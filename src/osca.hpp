#pragma once

#include "kernel.hpp"

namespace osca {

class Jobs final {
  public:
    using JobFunc = auto (*)(void*) -> void;

    static auto constexpr JOB_QUEUE_SIZE = 256u;
    static auto constexpr JOB_DATA_SIZE = CACHE_LINE_SIZE - sizeof(JobFunc);

  private:
    struct Entry {
        JobFunc func;
        u8 data[CACHE_LINE_SIZE - sizeof(JobFunc)];
    };

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
    template <typename T> auto add(JobFunc func, T const& data) -> void {
        static_assert(sizeof(T) <= JOB_DATA_SIZE);

        auto i = head % JOB_QUEUE_SIZE;
        queue[i].func = func;
        memcpy(queue[i].data, &data, sizeof(T));
        head = head + 1;
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
            // got job
            auto& job = queue[t % JOB_QUEUE_SIZE];
            job.func(job.data);
        }

        // some other thread got the job
        // queue was not empty for sure
        return true;
    }
};

extern Jobs jobs;

} // namespace osca
