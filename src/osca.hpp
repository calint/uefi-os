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
    static auto constexpr JOB_DATA_SIZE = CACHE_LINE_SIZE - sizeof(Func);

    struct Entry {
        u8 data[CACHE_LINE_SIZE - sizeof(Func)];
        Func func;
    };

    static_assert(sizeof(Entry) == CACHE_LINE_SIZE);

    alignas(CACHE_LINE_SIZE) Entry queue_[QUEUE_SIZE];
    alignas(CACHE_LINE_SIZE) u32 head_;
    alignas(CACHE_LINE_SIZE) u32 tail_;
    alignas(CACHE_LINE_SIZE) u32 active_;
    [[maybe_unused]] u8 padding[CACHE_LINE_SIZE - sizeof(tail_)];

    // note: a job is one cache line in size and aligned on cache lines
    //       `head`, `tail` and `active` on different cache lines avoiding false
    //       sharing
    //       padding to make `active` be only variable on the cache line

  public:
    // called from only 1 thread
    // copies data into the job
    // blocks while queue is full
    // data size must not be greater than 56 bytes (cach line size - func ptr)
    template <is_job T> auto add(T job) -> void {
        static_assert(sizeof(T) <= JOB_DATA_SIZE);

        // if queue is full wait
        while (head_ - tail_ >= QUEUE_SIZE) {
            asm volatile("pause");
        }

        auto i = head_ % QUEUE_SIZE;
        queue_[i].func = [](void* data) { ptr<T>(data)->run(); };
        memcpy(queue_[i].data, &job, sizeof(T));

        // tell compiler: "finish all previous writes before moving on"
        // making sure job data is written before head is increased
        // x86_64 ensures that store-store is not reordered
        asm volatile("" ::: "memory");

        head_ += 1;
    }

    // called from multiple threads
    // returns:
    //   true if queue was not empty (even if compare and exchange failed)
    //   false if queue was empty for sure
    auto run_next() -> bool {
        auto t = tail_;
        if (t == head_) {
            // is empty
            return false;
        }

        if (atomic_compare_exchange(&tail_, t, t + 1)) {
            atomic_add(&active_, 1);
            auto& entry = queue_[t % QUEUE_SIZE];
            entry.func(entry.data);
            atomic_add(&active_, -1);
            return true;
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
