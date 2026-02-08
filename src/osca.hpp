#pragma once

#include "atomic.hpp"
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
//   * try_add(), add(): single producer thread only
//   * run_next(): multiple consumer threads safe
//   * wait_idle(): safe from producer thread, blocks until all jobs complete
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

    struct Entry {
        u8 data[JOB_SIZE];
        Func func;
        u32 sequence;
        [[maybe_unused]] u32 padding;
    };

    static_assert(sizeof(Entry) == kernel::core::CACHE_LINE_SIZE);

    // note: different cache lines avoiding false sharing

    // job storage:
    // * single producer writes
    // * multiple consumers read only after claiming via tail
    alignas(kernel::core::CACHE_LINE_SIZE) Entry queue_[QueueSize];

    // read and written by producer
    alignas(kernel::core::CACHE_LINE_SIZE) u32 head_;

    // modified atomically by consumers
    alignas(kernel::core::CACHE_LINE_SIZE) u32 tail_;

    // read by producer written by consumers
    alignas(kernel::core::CACHE_LINE_SIZE) u32 completed_;

    // make sure `completed_` is alone on cache line
    [[maybe_unused]] u8
        padding[kernel::core::CACHE_LINE_SIZE - sizeof(completed_)];

  public:
    // safe to run while threads are running attempting `run_next`
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
        static_assert(sizeof(T) <= JOB_SIZE);

        auto& entry = queue_[head_ % QueueSize];

        // (1) paired with release (2)
        if (atomic::load_acquire(&entry.sequence) != head_) {
            // slot is not free from the previous lap
            return false;
        }

        // prepare slot
        entry.func = [](void* data) { ptr<T>(data)->run(); };
        *ptr<T>(entry.data) = {args...};
        ++head_;

        // hand over the slot to be run
        // (3) paired with acquire (4)
        atomic::store_release(&entry.sequence, head_);

        return true;
    }

    // called from producer
    // blocks while queue is full
    template <is_job T, typename... Args> auto add(Args&&... args) -> void {
        while (!try_add<T>(args...)) {
            kernel::core::pause();
        }
    }

    // called from multiple consumers
    // returns:
    //   true if job was run
    //   false if no job was run
    auto run_next() -> bool {
        // optimistic read; job data visible at (4), claimed at (7)
        auto t = atomic::load_relaxed(&tail_);
        while (true) {
            auto& entry = queue_[t % QueueSize];

            // (4) paired with release (3)
            auto seq = atomic::load_acquire(&entry.sequence);
            if (seq != t + 1) {
                // note: ABA issue might have happened where a competing thread
                //       claimed, and completed a job; in use case this is
                //       highly unlikely and ok since the core will issue a
                //       pause and `run_next` again
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

extern Jobs<256> jobs;

} // namespace osca
