#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

namespace detail {
// tests/negative_spsc.cpp weakens this to show TSan catching the race.
#ifdef ATOM_SPSC_WEAK_PUBLISH
inline constexpr std::memory_order kSpscPublish = std::memory_order_relaxed;
#else
inline constexpr std::memory_order kSpscPublish = std::memory_order_release;
#endif
}  // namespace detail

// No padding, no cached indices: kept for comparison.
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

    template <class U>
    bool try_push(U&& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // acquire: consumer is done reading the slot we reuse.
        if (tail - head_.load(std::memory_order_acquire) == capacity_) return false;
        buffer_[tail & mask_] = std::forward<U>(value);
        tail_.store(tail + 1, detail::kSpscPublish);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // acquire: slot contents are visible.
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = std::move(buffer_[head & mask_]);
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

// Padded, with cached copies of the other side's index.
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

    template <class U>
    bool try_push(U&& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == capacity_) {
            // acquire: consumer is done reading the slot we reuse.
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == capacity_) return false;
        }
        buffer_[tail & mask_] = std::forward<U>(value);
        tail_.store(tail + 1, detail::kSpscPublish);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            // acquire: slot contents are visible.
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false;
        }
        out = std::move(buffer_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    alignas(kCacheLine) const std::size_t capacity_;
    const std::size_t          mask_;
    const std::unique_ptr<T[]> buffer_;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer
    std::size_t                                  tail_cache_ = 0;

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer
    std::size_t                                  head_cache_ = 0;
};

}  // namespace atom
