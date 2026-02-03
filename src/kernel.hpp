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

struct FrameBuffer {
    u32* pixels;
    u32 width;
    u32 height;
    u32 stride;
};

extern FrameBuffer frame_buffer;

struct MemoryMap {
    void* buffer;
    u64 size;
    u64 descriptor_size;
    u32 descriptor_version;
};

extern MemoryMap memory_map;

struct KeyboardConfig {
    u32 gsi;
    u32 flags;
};

extern KeyboardConfig keyboard_config;

struct APIC {
    u32 volatile* io;
    u32 volatile* local;
};

extern APIC apic;

struct Core {
    u8 apic_id;
};

extern Core cores[];
extern u8 core_count;

struct Heap {
    void* start;
    u64 size;
};

extern Heap heap;

auto constexpr CACHE_LINE_SIZE = 64u;
// note: almost all modern x86_64 processors (intel and amd)

auto inline outb(u16 port, u8 val) -> void {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

auto inline inb(u16 port) -> u8 {
    u8 result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

extern "C" auto inline memset(void* s, i32 c, u64 n) -> void* {
    void* original_s = s;
    asm volatile("rep stosb" : "+D"(s), "+c"(n) : "a"(u8(c)) : "memory");
    return original_s;
}

extern "C" auto inline memcpy(void* dest, void const* src, u64 count) -> void* {
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(count) : : "memory");
    return dest;
}

auto inline serial_print(char const* s) -> void {
    while (*s) {
        outb(0x3f8, u8(*s++));
    }
}

auto inline serial_print_hex(u64 val) -> void {
    u8 constexpr hex_chars[] = "0123456789ABCDEF";
    for (auto i = 60; i >= 0; i -= 4) {
        outb(0x3f8, hex_chars[(val >> i) & 0xf]);
        if (i != 0 && (i % 16 == 0)) {
            outb(0x3f8, '_');
        }
    }
}

auto inline serial_print_dec(u64 val) -> void {
    // case for zero
    if (val == 0) {
        outb(0x3f8, '0');
        return;
    }

    // u64 max is 20 digits
    u8 buffer[21];
    auto i = 0u;

    // extract digits in reverse order
    while (val > 0) {
        buffer[i] = u8('0' + (val % 10));
        val /= 10;
        ++i;
    }

    // print the buffer backwards to show digits in correct order
    while (i > 0) {
        --i;
        outb(0x3f8, buffer[i]);
    }
}

auto inline serial_print_hex_byte(u8 val) -> void {
    u8 constexpr hex_chars[] = "0123456789ABCDEF";
    for (auto i = 4; i >= 0; i -= 4) {
        outb(0x3f8, hex_chars[(val >> i) & 0xf]);
    }
}

auto inline atomic_compare_exchange(u32* target, u32& expected, u32 desired)
    -> bool {
    return __atomic_compare_exchange_n(
        target,           // Pointer to the object to modify
        &expected,        // Pointer to the value we expect to find
        desired,          // The value we want to write if expected matches
        false,            // 'weak' = false (use strong version/LOCK prefix)
        __ATOMIC_SEQ_CST, // Success memory order
        __ATOMIC_SEQ_CST  // Failure memory order
    );
}

auto inline atomic_add(i32* target, i32 delta) -> void {
    __atomic_fetch_add(target, delta, __ATOMIC_SEQ_CST);
}

auto inline atomic_add_release(i32* target, i32 delta) -> void {
    __atomic_fetch_add(target, delta, __ATOMIC_RELEASE);
}

auto inline atomic_add_relaxed(i32* target, i32 delta) -> void {
    __atomic_fetch_add(target, delta, __ATOMIC_RELAXED);
}

auto inline atomic_load_acquire(u32 const* target) -> u32 {
    return __atomic_load_n(target, __ATOMIC_ACQUIRE);
}

auto inline atomic_load_relaxed(u32 const* target) -> u32 {
    return __atomic_load_n(target, __ATOMIC_RELAXED);
}

auto inline atomic_store_release(u32* target, u32 val) -> void {
    __atomic_store_n(target, val, __ATOMIC_RELEASE);
}

auto inline atomic_store_relaxed(u32* target, u32 val) -> void {
    __atomic_store_n(target, val, __ATOMIC_RELAXED);
}

auto inline pause() -> void { __builtin_ia32_pause(); }

template <typename T> auto inline ptr(void* p) -> T* {
    return reinterpret_cast<T*>(p);
}

template <typename T> auto inline ptr(void const* p) -> T const* {
    return reinterpret_cast<T const*>(p);
}

auto inline interrupts_enable() -> void { asm volatile("sti"); }

[[noreturn]] auto kernel_start() -> void;

namespace osca {

[[noreturn]] auto start() -> void;
[[noreturn]] auto run_core(u32 core_index) -> void;
auto on_keyboard(u8 scancode) -> void;
auto on_timer() -> void;

} // namespace osca
