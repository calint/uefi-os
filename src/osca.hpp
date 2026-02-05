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
//   * queue capacity: configurable through template argument (power of 2)
//
template <u32 QueueSize = 256> class Jobs final {
    using Func = auto (*)(void*) -> void;

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
    alignas(CACHE_LINE_SIZE) Entry queue_[QueueSize];

    // read and written by producer
    alignas(CACHE_LINE_SIZE) u32 head_;

    // modified atomically by consumers
    alignas(CACHE_LINE_SIZE) u32 tail_;

    union State {
        struct {
            // increased by consumers
            u32 completed; // low 32 bits (little endian)

            // increased by producer
            u32 submitted; // high 32 bits
        };
        u64 raw;
    } state_;

    // note: accessing `state_` atomic raw and bits through union is:
    // iso c++ standard  ub      mixed-size overlapping atomics are undefined.
    // x86_64 hardware   defined hardware guarantees cache-line coherence
    // gcc/clang         safe    built-ins handle the aliasing correctly
#if !defined(__x86_64__) || !(defined(__GNUC__) || defined(__clang__))
    static_assert(false, "this implementation requires x86_64 and gcc/clang");
#endif

    // make sure `state_` is alone on cache line
    [[maybe_unused]] u8 padding[CACHE_LINE_SIZE - sizeof(state_)];

  public:
    auto init() -> void {
        for (auto i = 0u; i < QueueSize; ++i) {
            queue_[i].sequence = i;
        }
        head_ = 0;
        tail_ = 0;
        state_.raw = 0;
    }

    // single producer only
    // copies data into the job queue
    // returns:
    //   true if job placed in queue
    //   false if queue was full
    template <is_job T> auto try_add(T job) -> bool {
        static_assert(sizeof(T) <= JOB_SIZE);

        auto h = atomic_load_relaxed(&head_);
        auto& entry = queue_[h % QueueSize];

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
        atomic_add_release(&state_.submitted, 1u);

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
            auto& entry = queue_[t % QueueSize];

            // (6) paired with release (5)
            auto seq = atomic_load_acquire(&entry.sequence);
            if (seq != t + 1) {
                // slot is not ready to run or queue is empty
                return false;
            }

            // definitive acquire of job data before execution
            // note: `weak` (true) because failure is retried in this loop
            // (8) acquires ownership from other consumers
            if (atomic_compare_exchange_acquire_relaxed(&tail_, t, t + 1,
                                                        true)) {
                entry.func(entry.data);

                // hand the slot back to the producer for the next lap
                // (2) paired with acquire (1)
                atomic_store_release(&entry.sequence, t + QueueSize);

                // increment completed (low 32 bits)
                // (7) paired with acquire (4)
                atomic_add_release(&state_.completed, 1u);
                return true;
            }
        }
    }

    // intended to be used in status displays etc
    auto active_count() const -> u32 {
        State s;
        s.raw = atomic_load_relaxed(&state_.raw);
        return s.submitted - s.completed;
    }

    // spin until all work is finished
    auto wait_idle() const -> void {
        while (true) {
            State state;
            // (4) paired with release (3) and (7)
            state.raw = atomic_load_acquire(&state_.raw);
            if (state.submitted == state.completed) {
                break;
            }
            pause();
        }
    }
};

extern Jobs<256> jobs;

} // namespace osca
