#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "atom/config.hpp"
#include "atom/queue_concept.hpp"

namespace atom {

// Many producers, many consumers: Dmitry Vyukov's bounded MPMC ring. Each cell carries an
// atomic sequence number that says whose turn the cell is:
//   seq == pos       empty, waiting for the producer that claims position pos
//   seq == pos + 1   filled, waiting for the consumer that claims position pos
//   seq == pos + cap empty again, waiting for the producer one lap later
// A thread claims a position with a CAS on enq_ / deq_, then owns that cell alone until it
// stores the next seq. Same progress caveat as MpscQueue: a producer paused after claiming a
// cell but before filling it holds up the consumer of that cell.
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
        for (std::size_t i = 0; i < capacity_; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);  // not shared yet
    }

    BoundedMpmcQueue(const BoundedMpmcQueue&)            = delete;
    BoundedMpmcQueue& operator=(const BoundedMpmcQueue&) = delete;

    template <class U>
    bool try_push(U&& value) {
        // relaxed: just a starting guess; the CAS below decides.
        std::size_t pos = enq_.load(std::memory_order_relaxed);
        Cell*       cell;
        for (;;) {
            cell = &cells_[pos & mask_];
            // acquire: pairs with the consumer's release of seq, so it is done reading the
            // cell's old value before we overwrite it.
            const std::size_t seq  = cell->seq.load(std::memory_order_acquire);
            const auto        diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                // relaxed: the CAS only hands out positions; the cell itself is ordered by seq.
                if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                               std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;  // the cell still holds last lap's item: full
            } else {
                pos = enq_.load(std::memory_order_relaxed);  // another producer took it
            }
        }
        cell->data = std::forward<U>(value);
        // release: the data write above is visible to the consumer that sees pos + 1.
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        std::size_t pos = deq_.load(std::memory_order_relaxed);  // starting guess
        Cell*       cell;
        for (;;) {
            cell = &cells_[pos & mask_];
            // acquire: pairs with the producer's release of seq, so the data is visible.
            const std::size_t seq  = cell->seq.load(std::memory_order_acquire);
            const auto        diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                               std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;  // not filled yet: empty
            } else {
                pos = deq_.load(std::memory_order_relaxed);  // another consumer took it
            }
        }
        out = std::move(cell->data);
        // release: our read of the data is done before the producer one lap later reuses it.
        cell->seq.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    alignas(kCacheLine) const std::size_t capacity_;
    const std::size_t             mask_;
    const std::unique_ptr<Cell[]> cells_;

    alignas(kCacheLine) std::atomic<std::size_t> enq_{0};  // producers
    alignas(kCacheLine) std::atomic<std::size_t> deq_{0};  // consumers
};

}  // namespace atom
