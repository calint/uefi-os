#pragma once

namespace atomic {

template <typename T>
auto inline compare_exchange_acquire_relaxed(T* target, T* expected, T desired,
                                             bool weak) -> bool {
    return __atomic_compare_exchange_n(
        target,           // pointer to the object to modify
        expected,         // pointer to the value we expect to find
        desired,          // the value we want to write if expected matches
        weak,             // 'weak' = false (use strong version/lock prefix)
        __ATOMIC_ACQUIRE, // success memory order
        __ATOMIC_RELAXED  // failure memory order
    );
}

template <typename T> auto inline add(T* target, T delta) -> void {
    __atomic_fetch_add(target, delta, __ATOMIC_SEQ_CST);
}

template <typename T> auto inline add_release(T* target, T delta) -> void {
    __atomic_fetch_add(target, delta, __ATOMIC_RELEASE);
}

template <typename T> auto inline add_relaxed(T* target, T delta) -> void {
    __atomic_fetch_add(target, delta, __ATOMIC_RELAXED);
}

template <typename T> auto inline load_acquire(T const* target) -> T {
    return __atomic_load_n(target, __ATOMIC_ACQUIRE);
}

template <typename T> auto inline load_relaxed(T const* target) -> T {
    return __atomic_load_n(target, __ATOMIC_RELAXED);
}

template <typename T> auto inline store_release(T* target, T val) -> void {
    __atomic_store_n(target, val, __ATOMIC_RELEASE);
}

template <typename T> auto inline store_relaxed(T* target, T val) -> void {
    __atomic_store_n(target, val, __ATOMIC_RELAXED);
}

}; // namespace atomic
