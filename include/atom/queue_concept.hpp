#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <type_traits>

namespace atom {

template <class Q>
concept ConcurrentQueue = requires(Q q, typename Q::value_type v) {
    { q.try_push(v) } -> std::same_as<bool>;
    { q.try_pop(v) } -> std::same_as<bool>;
};

template <class T>
concept QueueItem = std::default_initializable<T> && std::is_nothrow_move_assignable_v<T>;

namespace detail {
inline std::size_t ring_capacity(std::size_t requested) noexcept {
    return std::bit_ceil(requested < 2 ? std::size_t{2} : requested);
}
}  // namespace detail

}  // namespace atom
