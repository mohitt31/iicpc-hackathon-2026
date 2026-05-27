// spsc_queue.h — Single-Producer Single-Consumer lock-free ring buffer.
//
// Design: classic Vyukov-style SPSC. One producer thread pushes via
// try_push(). One consumer thread pops via try_pop(). No locks, no system
// calls on the hot path — just atomic loads and stores with carefully
// chosen memory orderings.
//
// Why this exists in our bot:
//   The hot loop currently calls hdr_record_value() + hdr_record_corrected_value()
//   inline when an ack arrives. Each call is ~50-100ns of work. At our
//   current rate (10k acks/sec per bot) that's ~0.1% of cycle time — not
//   worth offloading. At production rates (1M+ acks/sec) it becomes 10%
//   of CPU on the latency-critical thread. The right architecture is to
//   push the (latency, interval) tuple onto a lock-free ring in the hot
//   path (single atomic store, ~2-5ns) and let a cold consumer thread
//   absorb the HDR cost.
//
//   This file implements that ring. The bot exposes a --use-spsc flag
//   to enable this offload path for benchmarking and demonstration.
//
// Cache line layout (critical for performance):
//   The producer touches head_ on every push. The consumer touches tail_
//   on every pop. If they shared a cache line, every push would invalidate
//   the consumer's cache and vice versa (false sharing). We force each
//   atomic onto its own 64-byte cache line with alignas(64).
//
// Memory ordering proof sketch (Vyukov / cppreference SPSC pattern):
//   Producer (try_push):
//     1. Read tail_ with ACQUIRE — sees consumer's release writes,
//        so we know the slot at head_ is free (consumer past it).
//     2. Write the slot.
//     3. Store head_ with RELEASE — publishes the slot write to the
//        consumer. After this store is visible, the slot data is too.
//   Consumer (try_pop):
//     1. Read head_ with ACQUIRE — synchronizes with producer's
//        release store, so the slot data the producer wrote is visible.
//     2. Read the slot.
//     3. Store tail_ with RELEASE — publishes consumer progress to
//        the producer, so producer's next acquire of tail_ sees it.
//   This pattern is C++11-compliant and the canonical safe SPSC.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hft {

template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity > 1,
        "Capacity must be > 1");
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2 for fast bitmask modulo");
    static_assert(std::is_trivially_copyable_v<T>,
        "T must be trivially copyable so slot writes are atomic-safe");

public:
    // Producer-only state. The producer thread writes head_ frequently;
    // putting it on its own cache line means consumer reads don't
    // invalidate the producer's cache line.
    alignas(64) std::atomic<std::size_t> head_{0};

    // Consumer-only state. Same false-sharing logic as head_.
    alignas(64) std::atomic<std::size_t> tail_{0};

    // The slot array. On its own cache line region so it doesn't share
    // with the atomic indices.
    alignas(64) T buffer_[Capacity];

    // Non-copyable, non-movable. The queue lives in one place.
    SPSCQueue() = default;
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    // try_push — called by the SINGLE producer thread only.
    // Returns false if the queue is full (consumer hasn't kept up).
    // Hot path; designed to be inlined.
    inline bool try_push(const T& value) noexcept {
        // Relaxed: head_ is touched only by us, the producer.
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next_head = (head + 1) & (Capacity - 1);

        // Acquire: synchronize with consumer's release store to tail_.
        // If consumer's most recent pop is visible to us, we know that
        // slot is free for reuse.
        if (__builtin_expect(next_head == tail_.load(std::memory_order_acquire), 0)) {
            return false;  // queue full
        }

        // Write the slot before publishing the index.
        buffer_[head] = value;

        // Release: any reader who later loads head_ with acquire will
        // see the slot write we just did. This is the publication.
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // try_pop — called by the SINGLE consumer thread only.
    // Returns false if the queue is empty.
    inline bool try_pop(T& out) noexcept {
        // Relaxed: tail_ is touched only by us, the consumer.
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // Acquire: synchronize with producer's release store to head_.
        // We see the producer's published slot writes through this load.
        if (__builtin_expect(tail == head_.load(std::memory_order_acquire), 0)) {
            return false;  // queue empty
        }

        // Read the slot (safe — producer's write is visible due to acquire).
        out = buffer_[tail];

        // Release: producer's next acquire load of tail_ will see this,
        // freeing the slot for reuse.
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    // Approximate size — usable for monitoring, NOT for synchronization
    // decisions. The producer and consumer indices may both have changed
    // between the two loads here, so the result is a snapshot in time.
    inline std::size_t approx_size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & (Capacity - 1);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }
};

}  // namespace hft
