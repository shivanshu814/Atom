#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <type_traits>

namespace atom {

// The interface every queue shares, so the stress tests and benchmarks are written once.
// try_push returns false when the queue is full; try_pop returns false when there is nothing
// to take right now. Neither ever blocks.
template <class Q>
concept ConcurrentQueue = requires(Q q, typename Q::value_type v) {
    { q.try_push(v) } -> std::same_as<bool>;
    { q.try_pop(v) } -> std::same_as<bool>;
};

// What the ring-based queues need from T: slots are default-constructed up front, and
// items move in and out by assignment. Moving out must not throw, or a failed pop could
// leave a slot half-taken.
template <class T>
concept QueueItem = std::default_initializable<T> && std::is_nothrow_move_assignable_v<T>;

namespace detail {
// Rings index slots with `counter & mask`, which needs a power-of-two capacity.
inline std::size_t ring_capacity(std::size_t requested) noexcept {
    return std::bit_ceil(requested < 2 ? std::size_t{2} : requested);
}
}  // namespace detail

}  // namespace atom
