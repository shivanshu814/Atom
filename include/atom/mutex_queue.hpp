#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

// Baseline: a fixed ring behind one mutex.
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

    alignas(kCacheLine) std::mutex mutex_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
};

}  // namespace atom
