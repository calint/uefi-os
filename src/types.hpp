#pragma once

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8 = char;
using i16 = short;
using i32 = int;
using i64 = long long;
using uptr = u64;
using f32 = float;
using f64 = double;

// short hands for `reinterpret_cast`

template <typename T> auto inline ptr(void* p) -> T* {
    return static_cast<T*>(p);
}

template <typename T> auto inline ptr(void const* p) -> T const* {
    return static_cast<T const*>(p);
}

template <typename T> auto inline ptr(uptr p) -> T* {
    return reinterpret_cast<T*>(p);
}

// concepts

template <typename T, typename U>
concept is_same = __is_same(T, U);

template <typename T>
concept is_trivially_copyable = __is_trivially_copyable(T);

template <typename T>
concept is_constructible = __is_constructible(T);

template <typename T>
concept is_copy_constructible = __is_constructible(T, T const&);

template <typename T>
concept is_move_constructible = __is_constructible(T, T&&);

template <typename T>
concept is_destructible = __is_destructible(T);

template <typename T>
concept is_lvalue_reference = __is_lvalue_reference(T);

// perfect forwarding

template <typename T> struct remove_reference {
    using type = T;
};

template <typename T> struct remove_reference<T&> {
    using type = T;
};

template <typename T> struct remove_reference<T&&> {
    using type = T;
};

template <typename T>
auto constexpr fwd(typename remove_reference<T>::type& t) noexcept -> T&& {
    return static_cast<T&&>(t);
}

template <typename T>
auto constexpr fwd(typename remove_reference<T>::type&& t) noexcept -> T&& {
    static_assert(!is_lvalue_reference<T>);
    return static_cast<T&&>(t);
}
