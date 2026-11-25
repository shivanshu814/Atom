#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

// Baseline: a fixed-capacity ring guarded by one std::mutex. It has the same bounded-ring
// shape as the lock-free rings and never allocates after construction, so a benchmark
// against it measures the synchronisation, not the container. It is correct by
// construction, which also makes it the reference for the stress tests: if a test passes
// here and fails on a lock-free queue, the bug is in that queue, not in the test.
template <QueueItem T>
class MutexQueue {
public:
    using value_type = T;

    explicit MutexQueue(std::size_t capacity)
        : capacity_(detail::ring_capacity(capacity)),
          mask_(capacity_ - 1),
          buffer_(std::make_unique<T[]>(capacity_)) {}

    MutexQueue(const MutexQueue&)            = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;

    // Takes a forwarding reference so a failed push leaves the caller's item untouched.
    template <class U>
    bool try_push(U&& value) {
        std::lock_guard lock(mutex_);
        if (tail_ - head_ == capacity_) return false;
        buffer_[tail_ & mask_] = std::forward<U>(value);
        ++tail_;
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard lock(mutex_);
        if (tail_ == head_) return false;
        out = std::move(buffer_[head_ & mask_]);
        ++head_;
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t          capacity_;
    const std::size_t          mask_;
    const std::unique_ptr<T[]> buffer_;

    // Everything below is written under the lock by every thread, so it shares one line;
    // padding it away from the read-only fields above keeps those from bouncing too.
    alignas(kCacheLine) std::mutex mutex_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
};

}  // namespace atom
