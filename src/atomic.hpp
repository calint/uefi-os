#pragma once

#include "types.hpp"

namespace atomic {

auto constexpr RELAXED = __ATOMIC_RELAXED;
auto constexpr ACQUIRE = __ATOMIC_ACQUIRE;
auto constexpr RELEASE = __ATOMIC_RELEASE;
auto constexpr ACQ_REL = __ATOMIC_ACQ_REL;
auto constexpr SEQ_CST = __ATOMIC_SEQ_CST;

template <typename T>
auto inline compare_exchange(T* const target, T* const expected,
                             T const desired, bool const weak,
                             i32 const mem_order_success,
                             i32 const mem_order_failure) -> bool {
    // note: failure memory order cannot be stronger than success
    return __atomic_compare_exchange_n(target, expected, desired, weak,
                                       mem_order_success, mem_order_failure);
}

// returns the previous value
template <typename T>
auto inline add(T* const target, T const delta, i32 const mem_order) -> T {
    return __atomic_fetch_add(target, delta, mem_order);
}

template <typename T>
auto inline sub(T* const target, T const delta, i32 const mem_order) -> T {
    return __atomic_fetch_sub(target, delta, mem_order);
}

template <typename T>
auto inline exchange(T* const target, T const val, i32 const mem_order) -> T {
    return __atomic_exchange_n(target, val, mem_order);
}

template <typename T>
auto inline load(T const* const target, i32 const mem_order) -> T {
    return __atomic_load_n(target, mem_order);
}

template <typename T>
auto inline store(T* const target, T const val, i32 const mem_order) -> void {
    __atomic_store_n(target, val, mem_order);
}

} // namespace atomic
