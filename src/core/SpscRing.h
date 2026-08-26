#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mm {

/// Single-producer single-consumer lock-free ring buffer (the textbook Lamport queue):
/// one thread calls push(), one thread calls pop(), never the same thread for both roles.
/// Head and tail are each written by exactly ONE side; the release store on the writer's
/// index paired with the acquire load on the reader's side is what makes the element data
/// visible before the index move — the whole correctness argument, and why neither side
/// ever locks or waits.
///
/// Overflow policy is DROP-NEWEST: push() accepts what fits and reports how much. The
/// alternative (drop-oldest) requires the producer to advance the consumer's index — a
/// second writer on `tail`, which breaks the single-writer invariant the lock-freedom
/// rests on. For the audio-capture use the trade is right anyway: overflow only happens
/// when the consumer stalled (nothing is rendering), and the backlog self-drains once it
/// resumes.
///
/// N must be a power of two (indices wrap by masking); one slot is sacrificed so a full
/// ring is distinguishable from an empty one without a separate count.
template <typename T, uint32_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0 && N > 1, "capacity must be a power of two");

public:
    /// Producer side. Copies up to `count` elements from `src`; returns how many were
    /// accepted (fewer than `count` when the ring is near full — drop-newest).
    size_t push(const T* src, size_t count) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const uint32_t free = N - 1 - (head - tail);
        const size_t n = count < free ? count : free;
        for (size_t i = 0; i < n; i++) buf_[(head + i) & (N - 1)] = src[i];
        head_.store(head + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    /// Consumer side. Copies up to `max` elements into `dst`; returns how many were read.
    size_t pop(T* dst, size_t max) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t avail = head - tail;
        const size_t n = max < avail ? max : avail;
        for (size_t i = 0; i < n; i++) dst[i] = buf_[(tail + i) & (N - 1)];
        tail_.store(tail + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    /// Elements currently queued, as the consumer sees it (approximate under concurrency,
    /// exact when only one side is active). Display/diagnostic use.
    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    T buf_[N];
    // Free-running 32-bit indices (wrap by masking); unsigned overflow is defined and the
    // head-tail subtraction stays correct across it.
    std::atomic<uint32_t> head_{0};   // written only by the producer
    std::atomic<uint32_t> tail_{0};   // written only by the consumer
};

}  // namespace mm
