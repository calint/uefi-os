#pragma once

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8 = char;
using i16 = short;
using i32 = int;
using i64 = long long;
using f32 = float;
using f64 = double;
using uptr = u64;

// shorthand for pointer casts

template <typename T> auto constexpr inline ptr(void* const p) -> T* {
    return static_cast<T*>(p);
}

template <typename T>
auto constexpr inline ptr(void const* const p) -> T const* {
    return static_cast<T const*>(p);
}

template <typename T> auto constexpr inline ptr(uptr const p) -> T* {
    return reinterpret_cast<T*>(p);
}

template <typename T>
auto constexpr inline ptr_offset(void const* const p, u64 const bytes)
    -> T const* {
    return ptr<T>(uptr(p) + bytes);
}

template <typename T>
auto constexpr inline ptr_offset(void* const p, u64 const bytes) -> T* {
    return ptr<T>(uptr(p) + bytes);
}

template <typename T>
auto constexpr inline ptr_offset(uptr const p, u64 const bytes) -> T* {
    return ptr<T>(p + bytes);
}

// concepts

template <typename T, typename U>
concept is_same = __is_same(T, U);

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
auto constexpr inline fwd(typename remove_reference<T>::type& t) noexcept
    -> T&& {
    return static_cast<T&&>(t);
}

template <typename T>
auto constexpr inline fwd(typename remove_reference<T>::type&& t) noexcept
    -> T&& {
    return static_cast<T&&>(t);
}
