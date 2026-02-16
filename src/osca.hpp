#pragma once

#include "atomic.hpp"
#include "kernel.hpp"
#include "types.hpp"

namespace osca {

namespace queue {

template <typename T>
concept is_job = requires(T t) {
    { t.run() } -> is_same<void>;
};

//
// single-producer, multi-consumer lock-free job queue
//
// thread safety:
//  * try_add(), add(): single producer thread only
//  * run_next(): multiple consumer threads safe
//  * wait_idle(): safe from producer thread
//
// constraints:
//  * max job parameters size: 48 bytes
//  * queue capacity: configurable through template argument (power of 2)
//  * an interrupt that adds jobs must not happen in producer thread
//
template <u32 QueueSize = 256> class Spmc final {
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
        u32 unused;
    };

    static_assert(sizeof(Entry) == kernel::core::CACHE_LINE_SIZE);

    // note: different cache lines avoiding false sharing

    // producer reads and writes, consumers atomically read and write
    alignas(kernel::core::CACHE_LINE_SIZE) Entry queue_[QueueSize];

    // producer reads and writes
    alignas(kernel::core::CACHE_LINE_SIZE) u32 head_;

    // consumers atomically read and write
    alignas(kernel::core::CACHE_LINE_SIZE) u32 tail_;

    // producer atomically reads, consumers atomically write
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
        if (atomic::load(&entry.sequence, atomic::ACQUIRE) != head_) {
            // slot is not free from the previous lap
            return false;
        }

        // prepare slot
        new (entry.data) T{fwd<Args>(args)...};
        entry.func = [](void* data) {
            auto* const p = ptr<T>(data);
            p->run();
            p->~T();
        };
        ++head_;

        // hand over the slot to be run
        // (3) paired with acquire (4)
        atomic::store(&entry.sequence, head_, atomic::RELEASE);

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
        auto t = atomic::load(&tail_, atomic::RELAXED);
        while (true) {
            auto& entry = queue_[t % QueueSize];

            // (4) paired with release (3)
            auto const seq = atomic::load(&entry.sequence, atomic::ACQUIRE);
            // note: acquire ensures job data written by producer is visible
            //       here

            // signed difference correctly handles u32 wrap-around
            auto const diff = i32(seq - (t + 1));

            if (diff < 0) {
                // job not ready (producer hasn't reached here)
                return false;
            }

            if (diff > 0) {
                // `t` is stale, refresh and loop
                t = atomic::load(&tail_, atomic::RELAXED);
                continue;
            }

            // job is ready to be run, try to claim it

            // (7) atomically claims this job from competing consumers
            // note: `weak` (true) because failure is retried in this loop
            // note: success is relaxed because data visibility is already
            //       guaranteed by the acquire on `sequence` at (4)
            if (atomic::compare_exchange(&tail_, &t, t + 1, true,
                                         atomic::RELAXED, atomic::RELAXED)) {
                entry.func(entry.data);

                // hand the slot back to the producer for the next lap
                // (2) paired with acquire (1)
                atomic::store(&entry.sequence, t + QueueSize, atomic::RELEASE);

                // increment completed and release job side-effects for
                // `wait_idle`
                // (5) paired with acquire (6)
                atomic::add(&completed_, 1u, atomic::RELEASE);

                return true;
            }

            // job was taken by competing consumer or spurious fail happened,
            // try again without pause
            // note: `t` is now the value of what `tail_` was at compare
        }
    }

    // called from producer
    // intended to be used in status displays etc
    auto active_count() const -> u32 {
        auto const completed = atomic::load(&completed_, atomic::RELAXED);
        return head_ - completed;
    }

    // called from producer
    // spin until all work is finished
    auto wait_idle() const -> void {
        // note: since this is the producer, `head_` will not change while in
        // this loop

        // (6) paired with release (5)
        // note: acquire is required to see job memory side-effects
        while (head_ != atomic::load(&completed_, atomic::ACQUIRE)) {
            kernel::core::pause();
        }
    }
};

//
// multi-producer, multi-consumer lock-free job queue
//
// thread safety:
//  * try_add(), add(): multiple producer threads safe
//  * run_next(): multiple consumer threads safe
//
// constraints:
//  * max job parameters size: 48 bytes
//  * queue capacity: configurable through template argument (power of 2)
//  * safe to be interrupted and interrupt to add job
//
template <u32 QueueSize = 256> class Mpmc final {
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
        u32 unused;
    };

    static_assert(sizeof(Entry) == kernel::core::CACHE_LINE_SIZE);

    // note: different cache lines avoiding false sharing

    // producer reads and writes, consumers atomically read and write
    alignas(kernel::core::CACHE_LINE_SIZE) Entry queue_[QueueSize];

    // producers atomically read and write
    alignas(kernel::core::CACHE_LINE_SIZE) u32 head_;

    // consumers atomically read and write
    alignas(kernel::core::CACHE_LINE_SIZE) u32 tail_;

    // producer atomically reads, consumers atomically write
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

    // called from multiple producers
    // creates job into the queue
    // returns:
    //   true if job placed in queue
    //   false if queue was full
    template <is_job T, typename... Args> auto try_add(Args&&... args) -> bool {
        static_assert(sizeof(T) <= JOB_SIZE, "job too large for queue slot");

        // optimistic read; job data visible at (1) and claimed at (8)
        // note: if `h` is stale either sequence check or CAS fails safely
        auto h = atomic::load(&head_, atomic::RELAXED);

        while (true) {
            auto& entry = queue_[h % QueueSize];

            // (1) paired with release (2)
            auto const seq = atomic::load(&entry.sequence, atomic::ACQUIRE);

            // signed difference correctly handles u32 wrap-around
            auto const diff = i32(seq - h);

            if (diff > 0) {
                // `seq` is ahead of `h` -> competing producer took slot
                h = atomic::load(&head_, atomic::RELAXED);
                continue;
            }

            if (diff < 0) {
                // `seq` is behind `h` -> queue is full
                return false;
            }

            // `seq` is `h` -> slot is ready, try to claim it

            // (8) claim slot and release paired with (9)
            // note: success is relaxed because data is published later via
            //       `sequence`
            if (atomic::compare_exchange(&head_, &h, h + 1, true,
                                         atomic::RELAXED, atomic::RELAXED)) {
                // prepare slot
                new (entry.data) T{fwd<Args>(args)...};
                entry.func = [](void* data) {
                    auto* const p = ptr<T>(data);
                    p->run();
                    p->~T();
                };

                // hand over the slot to be run
                // (3) paired with acquire (4)
                atomic::store(&entry.sequence, h + 1, atomic::RELEASE);
                // note: release publishes job data and gives ownership to
                //       consumer

                return true;
            }

            // competing producer took slot
            // note: `h` is now what `head_` was at compare exchange
        }
    }

    // called from multiple producers
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
        auto t = atomic::load(&tail_, atomic::RELAXED);
        while (true) {
            auto& entry = queue_[t % QueueSize];

            // (4) paired with release (3)
            auto const seq = atomic::load(&entry.sequence, atomic::ACQUIRE);
            // note: acquire ensures job data written by producer is visible
            //       here

            // signed difference correctly handles u32 wrap-around
            auto const diff = i32(seq - (t + 1));

            if (diff < 0) {
                // job not ready (producer hasn't reached here)
                return false;
            }

            if (diff > 0) {
                // `t` is stale, refresh and loop
                t = atomic::load(&tail_, atomic::RELAXED);
                continue;
            }

            // job is ready to run, try to claim it

            // (7) atomically claims this job from competing consumers
            // note: `weak` (true) because failure is retried in this loop
            // note: success is relaxed because data visibility is already
            //       guaranteed by the acquire on `sequence` at (4)
            if (atomic::compare_exchange(&tail_, &t, t + 1, true,
                                         atomic::RELAXED, atomic::RELAXED)) {
                entry.func(entry.data);

                // hand the slot back to the producer for the next lap
                // (2) paired with acquire (1)
                atomic::store(&entry.sequence, t + QueueSize, atomic::RELEASE);
                // note: release makes the slot available for producer's next
                //       lap

                // increment completed and release job side-effects for
                // `wait_idle`
                // (5) paired with acquire (6)
                atomic::add(&completed_, 1u, atomic::RELEASE);

                return true;
            }

            // job was taken by competing consumer or spurious fail happened,
            // try again without pause
            // note: `t` is now the value of what `tail_` was at compare
        }
    }

    // intended to be used in status displays etc
    auto active_count() const -> u32 {
        auto const head = atomic::load(&head_, atomic::RELAXED);
        auto const completed = atomic::load(&completed_, atomic::RELAXED);
        return head - completed;
    }

    // spin until all work is finished
    auto wait_idle() const -> void {
        while (true) {
            auto const head = atomic::load(&head_, atomic::RELAXED);
            // note: relaxed is safe; thread sees its own prior additions

            // (6) paired with release (5)
            // note: acquire is required to see job memory side-effects
            auto const completed = atomic::load(&completed_, atomic::ACQUIRE);

            if (head == completed) {
                return;
            }

            kernel::core::pause();
        }
    }
};

} // namespace queue

queue::Mpmc<256> inline jobs;

} // namespace osca
