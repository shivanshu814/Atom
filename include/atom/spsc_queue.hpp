#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

namespace detail {
// The ordering of the store that publishes a filled slot. Only the negative test changes it:
// it builds with ATOM_SPSC_WEAK_PUBLISH to show that TSan catches the race a relaxed store
// creates (tests/negative_spsc.cpp).
#ifdef ATOM_SPSC_WEAK_PUBLISH
inline constexpr std::memory_order kSpscPublish = std::memory_order_relaxed;
#else
inline constexpr std::memory_order kSpscPublish = std::memory_order_release;
#endif
}  // namespace detail

// Single-producer single-consumer ring, the straightforward version (milestone M2): every
// call loads the other side's index, and nothing is padded. It is kept next to SpscQueue so
// the benchmark can show what the padding and the cached indices are worth.
//
// head_ and tail_ are monotonic counters; the slot is `counter & mask_`. Full is
// tail - head == capacity, empty is tail == head, so no slot is wasted.
template <QueueItem T>
class NaiveSpscQueue {
public:
    using value_type = T;

    explicit NaiveSpscQueue(std::size_t capacity)
        : capacity_(detail::ring_capacity(capacity)),
          mask_(capacity_ - 1),
          buffer_(std::make_unique<T[]>(capacity_)) {}

    NaiveSpscQueue(const NaiveSpscQueue&)            = delete;
    NaiveSpscQueue& operator=(const NaiveSpscQueue&) = delete;

    // Producer only.
    template <class U>
    bool try_push(U&& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only I write tail_
        // acquire: pairs with the consumer's release of head_, so it has finished reading
        // the slot I am about to overwrite.
        if (tail - head_.load(std::memory_order_acquire) == capacity_) return false;
        buffer_[tail & mask_] = std::forward<U>(value);
        // release: the slot write above is visible before the consumer can see the new tail.
        tail_.store(tail + 1, detail::kSpscPublish);
        return true;
    }

    // Consumer only.
    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);  // only I write head_
        // acquire: pairs with the producer's release of tail_, so the slot contents are visible.
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = std::move(buffer_[head & mask_]);
        // release: my read of the slot is done before the producer may reuse it.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t          capacity_;
    const std::size_t          mask_;
    const std::unique_ptr<T[]> buffer_;
    std::atomic<std::size_t>   head_{0};
    std::atomic<std::size_t>   tail_{0};
};

// Single-producer single-consumer ring (milestone M3). Wait-free on both sides.
//
// Two changes over NaiveSpscQueue:
//  1. Three cache lines: read-only fields, consumer-written fields, producer-written fields.
//     With head_ and tail_ on one line, every push invalidates the consumer's copy of that
//     line and every pop the producer's (false sharing).
//  2. Cached indices. Each side keeps a private copy of the other side's index and only
//     reloads the shared atomic when its copy says "full" or "empty". In steady state each
//     call touches only its own line.
template <QueueItem T>
class SpscQueue {
public:
    using value_type = T;

    explicit SpscQueue(std::size_t capacity)
        : capacity_(detail::ring_capacity(capacity)),
          mask_(capacity_ - 1),
          buffer_(std::make_unique<T[]>(capacity_)) {}

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer only.
    template <class U>
    bool try_push(U&& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only I write tail_
        if (tail - head_cache_ == capacity_) {
            // Looks full by my stale copy, so check the real head.
            // acquire: pairs with the consumer's release of head_, so it has finished
            // reading the slot I am about to overwrite.
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == capacity_) return false;
        }
        buffer_[tail & mask_] = std::forward<U>(value);
        // release: the slot write above is visible before the consumer can see the new tail.
        tail_.store(tail + 1, detail::kSpscPublish);
        return true;
    }

    // Consumer only.
    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);  // only I write head_
        if (head == tail_cache_) {
            // Looks empty by my stale copy, so check the real tail.
            // acquire: pairs with the producer's release of tail_, so the slot is visible.
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false;
        }
        out = std::move(buffer_[head & mask_]);
        // release: my read of the slot is done before the producer may reuse it.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    // Line 0: read by both sides on every call, never written after construction.
    alignas(kCacheLine) const std::size_t capacity_;
    const std::size_t          mask_;
    const std::unique_ptr<T[]> buffer_;

    // Line 1: written by the consumer.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t                                  tail_cache_ = 0;

    // Line 2: written by the producer. Being last, the class size rounds up to a whole
    // line, so a neighbouring object cannot share it either.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t                                  head_cache_ = 0;
};

}  // namespace atom
