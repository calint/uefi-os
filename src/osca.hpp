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
    static auto constexpr JOB_SIZE = CACHE_LINE_SIZE - sizeof(Func);

    struct Entry {
        u8 data[CACHE_LINE_SIZE - sizeof(Func)];
        Func func;
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
    // single producer only
    // copies data into the job queue
    // returns:
    //   true if job placed in queue
    //   false if queue wos full
    // data size must not be greater than 56 bytes (cache line size - func ptr)
    template <is_job T> auto try_add(T job) -> bool {
        static_assert(sizeof(T) <= JOB_SIZE);

        // if queue is full wait
        if (head_ - tail_ >= QUEUE_SIZE) {
            return false;
        }

        auto i = head_ % QUEUE_SIZE;
        queue_[i].func = [](void* data) { ptr<T>(data)->run(); };
        memcpy(queue_[i].data, &job, sizeof(T));

        // * tell compiler: "finish all previous writes before moving on"
        // * making sure job data is written before head is increased
        // * x86_64 ensures that store-store is not reordered
        asm volatile("" ::: "memory");

        head_ += 1;

        return true;
    }

    // single producer only
    // blocks while queue is full
    template <is_job T> auto inline add(T job) -> void {
        while (!try_add(job)) {
            asm volatile("pause");
        }
    }

    // multiple consumers
    // returns:
    //   true if queue was not empty (even if compare and exchange failed)
    //   false if queue was empty for sure
    auto run_next() -> bool {
        auto t = tail_;

        // load-load ordering is guaranteed on x86
        if (t == head_) {
            // is empty
            return false;
        }

        // claim slot using locked RMW
        if (atomic_compare_exchange(&tail_, t, t + 1)) {
            atomic_add(&active_, 1);
            auto& entry = queue_[t % QUEUE_SIZE];
            entry.func(entry.data);
            atomic_add(&active_, -1);
        }

        // some other thread got the job
        // queue was not empty for sure
        return true;
    }

    auto active_count() const -> u32 { return active_; }

    // Spin until all work is finished
    auto wait_idle() const -> void {
        while (true) {
            if (head_ == tail_ && active_ == 0) {
                break;
            }
            asm volatile("pause" ::: "memory");
        }
    }
};

extern Jobs jobs;

} // namespace osca
