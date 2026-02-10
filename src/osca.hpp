#pragma once

#include "atomic.hpp"
#include "kernel.hpp"
#include "types.hpp"

namespace osca {

template <typename T>
concept is_job = requires(T t) {
    { t.run() } -> is_same<void>;
};

//
// single-producer, multi-consumer lock-free job queue
//
// thread safety:
//   * try_add(), add(): single producer thread only
//   * run_next(): multiple consumer threads safe
//   * wait_idle(): safe from producer thread
//
// constraints:
//   * max job parameters size: 48 bytes
//   * queue capacity: configurable through template argument (power of 2)
//
template <u32 QueueSize = 256> class Jobs final {
    static_assert(
        (QueueSize & (QueueSize - 1)) == 0 && QueueSize > 1,
        "QueueSize must be a power of 2 for efficient modulo operations");

    using Func = auto (*)(void*) -> void;

    static auto constexpr JOB_SIZE =
        kernel::core::CACHE_LINE_SIZE - sizeof(Func) - 2 * sizeof(u32);

    struct alignas(kernel::core::CACHE_LINE_SIZE) Entry {
        u8 data[JOB_SIZE];
        Func func;
        u32 sequence;
        u32 padding;
        // note: pad so func remains 8-byte aligned and sequence 4-byte aligned
    };

    static_assert(sizeof(Entry) == kernel::core::CACHE_LINE_SIZE);

    // note: different cache lines avoiding false sharing

    // job storage:
    // * single producer writes
    // * multiple consumers read and write atomically after claiming via tail
    alignas(kernel::core::CACHE_LINE_SIZE) Entry queue_[QueueSize];

    // read and written by producer
    alignas(kernel::core::CACHE_LINE_SIZE) u32 head_;

    // modified atomically by consumers
    alignas(kernel::core::CACHE_LINE_SIZE) u32 tail_;

    // read by producer written by consumers
    alignas(kernel::core::CACHE_LINE_SIZE) u32 completed_;

    // make sure `completed_` is alone on cache line
    u8 padding[kernel::core::CACHE_LINE_SIZE - sizeof(completed_)];

  public:
    // safe to run while threads are running attempting `run_next` if assumed
    // zero initialized in data section
    auto init() -> void {
        head_ = 0;
        tail_ = 0;
        completed_ = 0;
        for (auto i = 0u; i < QueueSize; ++i) {
            queue_[i].sequence = i;
        }
    }

    // called from producer
    // creates job into the queue
    // returns:
    //   true if job placed in queue
    //   false if queue was full
    template <is_job T, typename... Args> auto try_add(Args&&... args) -> bool {
        static_assert(sizeof(T) <= JOB_SIZE, "job too large for queue slot");

        auto& entry = queue_[head_ % QueueSize];

        // (1) paired with release (2)
        if (atomic::load_acquire(&entry.sequence) != head_) {
            // slot is not free from the previous lap
            return false;
        }

        // prepare slot
        new (entry.data) T{fwd<Args>(args)...};
        entry.func = [](void* data) {
            auto p = ptr<T>(data);
            p->run();
            p->~T();
        };
        ++head_;

        // hand over the slot to be run
        // (3) paired with acquire (4)
        atomic::store_release(&entry.sequence, head_);

        return true;
    }

    // called from producer
    // blocks while queue is full
    template <is_job T, typename... Args> auto add(Args&&... args) -> void {
        while (!try_add<T>(fwd<Args>(args)...)) {
            kernel::core::pause();
        }
    }

    // called from multiple consumers
    // returns:
    //   true if job was run
    //   false if no job was run
    auto run_next() -> bool {
        // optimistic read; job data visible at (4), claimed at (7)
        // note: if `t` is stale, either sequence check or CAS will safely fail
        auto t = atomic::load_relaxed(&tail_);
        while (true) {
            auto& entry = queue_[t % QueueSize];

            // (4) paired with release (3)
            auto seq = atomic::load_acquire(&entry.sequence);
            if (seq != t + 1) {
                // job not ready or `t` stale; caller will retry
                return false;
            }

            // definitive acquire of job data before execution
            // note: `weak` (true) because failure is retried in this loop
            // (7) atomically claims this job from competing consumers
            if (atomic::compare_exchange_acquire_relaxed(&tail_, &t, t + 1,
                                                         true)) {
                entry.func(entry.data);

                // hand the slot back to the producer for the next lap
                // (2) paired with acquire (1)
                atomic::store_release(&entry.sequence, t + QueueSize);

                // increment completed
                // (5) paired with acquire (6)
                atomic::add_release(&completed_, 1u);

                return true;
            }

            // job was taken by competing core or spurious fail happened, try
            // again without pause
            // note: `t` is now the value of what `tail_` was at compare
        }
    }

    // called from producer
    // intended to be used in status displays etc
    auto active_count() const -> u32 {
        return head_ - atomic::load_relaxed(&completed_);
    }

    // called from producer
    // spin until all work is finished
    auto wait_idle() const -> void {
        // note: since this is the producer, `head_` will not change while in
        // this loop

        // (6) paired with release (5)
        while (head_ != atomic::load_acquire(&completed_)) {
            kernel::core::pause();
        }
    }
};

Jobs<256> inline jobs;
// note: 0 initialized

} // namespace osca
