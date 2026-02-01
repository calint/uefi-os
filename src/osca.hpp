#pragma once

#include "kernel.hpp"

namespace osca {

auto constexpr JOB_QUEUE_LEN = 256u;

class JobQueue final {
  public:
    using Job = auto (*)(void*) -> void;

  private:
    struct Entry {
        Job job;
        void* data;
    };

    volatile Entry queue[JOB_QUEUE_LEN];
    volatile u32 head;
    volatile u32 tail;

  public:
    // called from only 1 thread
    auto add(Job job, void* data) -> void {
        auto i = head % JOB_QUEUE_LEN;
        queue[i].job = job;
        queue[i].data = data;
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
            auto& job = queue[t % JOB_QUEUE_LEN];
            job.job(job.data);
        }

        // some other thread got the job
        // queue was not empty for sure
        return true;
    }
};

extern JobQueue job_queue;

} // namespace osca
