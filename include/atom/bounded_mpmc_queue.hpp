#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

// Vyukov's bounded MPMC ring. cell.seq: pos = empty, pos + 1 = full, pos + cap = next lap.
template <QueueItem T>
class BoundedMpmcQueue {
    struct Cell {
        std::atomic<std::size_t> seq{0};
        T                        data{};
    };

public:
    using value_type = T;

    explicit BoundedMpmcQueue(std::size_t capacity)
        : capacity_(detail::ring_capacity(capacity)),
          mask_(capacity_ - 1),
          cells_(std::make_unique<Cell[]>(capacity_)) {
        for (std::size_t i = 0; i < capacity_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    BoundedMpmcQueue(const BoundedMpmcQueue&)            = delete;
    BoundedMpmcQueue& operator=(const BoundedMpmcQueue&) = delete;

    template <class U>
    bool try_push(U&& value) {
        std::size_t pos = enq_.load(std::memory_order_relaxed);
        Cell*       cell;
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq  = cell->seq.load(std::memory_order_acquire);
            const auto        diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                // relaxed: the cell is ordered by seq, not by enq_.
                if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                               std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = enq_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::forward<U>(value);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        std::size_t pos = deq_.load(std::memory_order_relaxed);
        Cell*       cell;
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq  = cell->seq.load(std::memory_order_acquire);
            const auto        diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                               std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = deq_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->data);
        cell->seq.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    alignas(kCacheLine) const std::size_t capacity_;
    const std::size_t             mask_;
    const std::unique_ptr<Cell[]> cells_;

    alignas(kCacheLine) std::atomic<std::size_t> enq_{0};
    alignas(kCacheLine) std::atomic<std::size_t> deq_{0};
};

}  // namespace atom
