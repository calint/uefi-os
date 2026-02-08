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

template <typename T> auto inline ptr(void* p) -> T* {
    return reinterpret_cast<T*>(p);
}

template <typename T> auto inline ptr(void const* p) -> T const* {
    return reinterpret_cast<T const*>(p);
}

template <typename T> auto inline ptr(uptr p) -> T* {
    return reinterpret_cast<T*>(p);
}
