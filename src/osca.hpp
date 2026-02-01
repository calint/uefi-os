#pragma once

#include "kernel.hpp"

namespace osca {

template <typename T, typename U>
concept is_same = __is_same(T, U);

template <typename T>
concept is_trivially_copyable = __is_trivially_copyable(T);

template <typename T>
concept runnable = requires(T t) {
    { t.run() } -> is_same<void>;
};

template <typename T>
concept job = runnable<T> && is_trivially_copyable<T>;

class Jobs final {
  public:
    static auto constexpr JOB_QUEUE_SIZE = 256u;

  private:
    using Func = auto (*)(void*) -> void;

    static auto constexpr JOB_DATA_SIZE = CACHE_LINE_SIZE - sizeof(Func);

    struct Entry {
        u8 data[CACHE_LINE_SIZE - sizeof(Func)];
        Func func;
    };

    static_assert(sizeof(Entry) == CACHE_LINE_SIZE);

    alignas(CACHE_LINE_SIZE) Entry queue[JOB_QUEUE_SIZE];
    alignas(CACHE_LINE_SIZE) u32 head;
    alignas(CACHE_LINE_SIZE) u32 tail;
    [[maybe_unused]] u8 padding[CACHE_LINE_SIZE - sizeof(tail)];

    // note: a job is one cache line in size and aligned on cache lines
    //       `head` and `tail` on different cache lines avoiding False Sharing
    //       padding to make sure `tail` is the only variable on cache line

  public:
    // called from only 1 thread
    // copies data into the job
    // data size must be less than cache line size - 8 (the function pointer)
    template <job T> auto add(T job) -> void {
        static_assert(sizeof(T) <= JOB_DATA_SIZE);

        auto i = head % JOB_QUEUE_SIZE;
        queue[i].func = [](void* data) { ptr<T>(data)->run(); };
        memcpy(queue[i].data, &job, sizeof(T));

        // tell compiler: "Finish all previous writes before moving on."
        // asm volatile("" ::: "memory");

        head += 1;
    }

    // called from multiple threads
    // returns:
    //   true if queue was not empty (even if compare and exchange failed)
    //   false if queue was empty for sure
    auto run_next() -> bool {
        auto t = tail;
        if (t == head) {
            // is empty
            return false;
        }

        if (atomic_compare_exchange(&tail, t, t + 1)) {
            auto& entry = queue[t % JOB_QUEUE_SIZE];
            entry.func(entry.data);
        }

        // some other thread got the job
        // queue was not empty for sure
        return true;
    }
};

extern Jobs jobs;

} // namespace osca
