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

//
// single-producer, multi-consumer lock-free job queue
//
// thread safety:
//   * try_add(), add(): single producer thread only (bsp)
//   * run_next(): multiple consumer threads safe
//   * wait_idle(): safe from producer, blocks until all jobs complete
//
// constraints:
//   * max job parameters size: 48 bytes
//   * queue capacity: 256 jobs
//
class Jobs final {
    using Func = auto (*)(void*) -> void;

    static auto constexpr QUEUE_SIZE = 256u;
    static auto constexpr JOB_SIZE =
        CACHE_LINE_SIZE - sizeof(Func) - 2 * sizeof(u32);

    struct Entry {
        u8 data[JOB_SIZE];
        Func func;
        u32 sequence;
        u32 unused;
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

    // used in `wait_idle`
    // high 32 bits: submitted_count
    // low  32 bits: completed_count
    alignas(CACHE_LINE_SIZE) u64 state_;

    // make sure `state_` is alone on cache line
    [[maybe_unused]] u8 padding[CACHE_LINE_SIZE - sizeof(state_)];

  public:
    auto init() -> void {
        for (auto i = 0u; i < QUEUE_SIZE; ++i) {
            queue_[i].sequence = i;
        }
        head_ = 0;
        tail_ = 0;
        state_ = 0;
    }

    // single producer only
    // copies data into the job queue
    // returns:
    //   true if job placed in queue
    //   false if queue was full
    template <is_job T> auto try_add(T job) -> bool {
        static_assert(sizeof(T) <= JOB_SIZE);

        auto h = atomic_load_relaxed(&head_);
        auto& entry = queue_[h % QUEUE_SIZE];

        // (1) paired with release (2)
        if (atomic_load_acquire(&entry.sequence) != h) {
            // slot is not free from the previous lap
            return false;
        }

        // prepare slot
        entry.func = [](void* data) { ptr<T>(data)->run(); };
        memcpy(entry.data, &job, sizeof(T));

        // increment submitted (high 32 bits)
        // (3) paired with acquire (4)
        atomic_add_release(&state_, 1ull << 32);

        // hand over the slot to be run
        // (5) paired with acquire (6)
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
    //   false if queue was empty or next job is not ready
    auto run_next() -> bool {
        while (true) {
            auto t = atomic_load_relaxed(&tail_);
            auto& entry = queue_[t % QUEUE_SIZE];

            // (6) paired with release (5)
            auto seq = atomic_load_acquire(&entry.sequence);
            if (seq != t + 1) {
                // slot is not ready to run or queue is empty
                return false;
            }

            // definitive acquire of job data before execution
            // note: `weak` (false) because failure is retried in this loop
            // (8) acquires ownership from other consumers
            if (atomic_compare_exchange_acquire_relaxed(&tail_, t, t + 1,
                                                        false)) {
                entry.func(entry.data);

                // hand the slot back to the producer for the next lap
                // (2) paired with acquire (1)
                atomic_store_release(&entry.sequence, t + QUEUE_SIZE);

                // increment completed (low 32 bits)
                // (7) paired with acquire (4)
                atomic_add_release(&state_, 1ull);
                return true;
            }
        }
    }

    // intended to be used in status displays etc
    auto active_count() const -> u32 {
        auto s = atomic_load_relaxed(&state_);
        return u32(s >> 32) - u32(s & 0xFFFFFFFF);
    }

    // spin until all work is finished
    auto wait_idle() const -> void {
        while (true) {
            // (4) paired with release (3) and (7)
            auto snapshot = atomic_load_acquire(&state_);
            auto submitted = u32(snapshot >> 32);
            auto completed = u32(snapshot & 0xffffffff);
            if (submitted == completed) {
                break;
            }
            pause();
        }
    }
};

extern Jobs jobs;

} // namespace osca
