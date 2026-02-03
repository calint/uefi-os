#pragma once

#include "kernel.hpp"

namespace osca {

template <typename T, typename U>
concept is_same = __is_same(T, U);

template <typename T>
concept is_trivially_copyable = __is_trivially_copyable(T);

template <typename T>
concept is_job = is_trivially_copyable<T> && requires(T t) {
    { t.run() } -> is_same<void>;
};

class Jobs final {
    using Func = auto (*)(void*) -> void;

    static auto constexpr QUEUE_SIZE = 256u; // power of 2
    static auto constexpr JOB_SIZE =
        CACHE_LINE_SIZE - sizeof(Func) - 2 * sizeof(u32);

    struct Entry {
        u8 data[JOB_SIZE];
        Func func;
        u32 sequence;
        u32 padding;
    };

    static_assert(sizeof(Entry) == CACHE_LINE_SIZE);

    // note: different cache lines avoiding false sharing

    // job storage:
    // * single producer writes
    // * consumers read only after claiming via tail
    alignas(CACHE_LINE_SIZE) Entry queue_[QUEUE_SIZE];

    // written by producer, read by consumers
    alignas(CACHE_LINE_SIZE) u32 head_;

    // modified atomically by consumers
    alignas(CACHE_LINE_SIZE) u32 tail_;

    // number of active running jobs
    // modified atomically by consumers
    alignas(CACHE_LINE_SIZE) u32 active_;

    // padding to make `active_` the only variable on cache line
    [[maybe_unused]] u8 padding[CACHE_LINE_SIZE - sizeof(active_)];

  public:
    auto init() -> void {
        for (auto i = 0u; i < QUEUE_SIZE; ++i) {
            queue_[i].sequence = i;
        }
    }

    // single producer only
    // copies data into the job queue
    // returns:
    //   true if job placed in queue
    //   false if queue wos full
    // data size must not be greater than 56 bytes (cache line size - func ptr)
    template <is_job T> auto try_add(T job) -> bool {
        static_assert(sizeof(T) <= JOB_SIZE);

        auto h = head_; // local to this thread
        auto& entry = queue_[h % QUEUE_SIZE];
        if (atomic_load_acquire(&entry.sequence) != h) {
            // job is still running from previous lap
            return false;
        }
        entry.func = [](void* data) { ptr<T>(data)->run(); };
        memcpy(entry.data, &job, sizeof(T));
        // mark entry as busy using sequence != head_
        atomic_store_release(&entry.sequence, h + 1);
        atomic_store_relaxed(&head_, h + 1);
        return true;
    }

    // single producer only
    // blocks while queue is full
    template <is_job T> auto inline add(T job) -> void {
        while (!try_add(job)) {
            pause();
        }
    }

    // multiple consumers
    // returns:
    //   true if job was run
    //   false if queue was empty or next job not complete
    auto run_next() -> bool {
        while (true) {
            auto t = atomic_load_relaxed(&tail_);
            auto h = atomic_load_acquire(&head_);

            if (t == h) {
                return false;
            }

            auto& entry = queue_[t % QUEUE_SIZE];
            auto seq = atomic_load_acquire(&entry.sequence);

            if (seq != t + 1) {
                return false;
            }

            if (atomic_compare_exchange(&tail_, t, t + 1)) {
                atomic_add_relaxed(ptr<i32>(&active_), 1);
                entry.func(entry.data);
                atomic_store_release(&entry.sequence, t + QUEUE_SIZE);
                atomic_add(ptr<i32>(&active_), -1);
                return true;
            }
        }
    }

    auto active_count() const -> u32 { return atomic_load_relaxed(&active_); }

    // spin until all work is finished
    auto wait_idle() const -> void {
        while (true) {
            auto h = atomic_load_acquire(&head_);
            auto t = atomic_load_acquire(&tail_);
            auto a = atomic_load_acquire(&active_);
            if (h == t && a == 0) {
                break;
            }
            pause();
        }
    }
};

extern Jobs jobs;

} // namespace osca
