#pragma once

#include "types.hpp"

namespace kernel {

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

struct Apic {
    u32 volatile* io;
    u32 volatile* local;
};

extern Apic apic;

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

auto inline outb(u16 port, u8 val) -> void {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

auto inline inb(u16 port) -> u8 {
    u8 result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

[[noreturn]] auto start() -> void;

} // namespace kernel

namespace kernel::serial {

auto inline print(char const* s) -> void {
    while (*s) {
        outb(0x3f8, u8(*s++));
    }
}

auto inline print_hex(u64 val) -> void {
    u8 constexpr hex_chars[] = "0123456789ABCDEF";
    for (auto i = 60; i >= 0; i -= 4) {
        outb(0x3f8, hex_chars[(val >> i) & 0xf]);
        if (i != 0 && (i % 16 == 0)) {
            outb(0x3f8, '_');
        }
    }
}

auto inline print_dec(u64 val) -> void {
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

auto inline print_hex_byte(u8 val) -> void {
    u8 constexpr hex_chars[] = "0123456789ABCDEF";
    for (auto i = 4; i >= 0; i -= 4) {
        outb(0x3f8, hex_chars[(val >> i) & 0xf]);
    }
}

} // namespace kernel::serial

namespace kernel::core {

auto constexpr CACHE_LINE_SIZE = 64u;
// note: almost all modern x86_64 processors (intel and amd)

auto inline pause() -> void { __builtin_ia32_pause(); }
auto inline interrupts_enable() -> void { asm volatile("sti"); }
auto inline interrupts_disable() -> void { asm volatile("cli"); }
auto inline halt() -> void { asm volatile("hlt"); }

} // namespace kernel::core

// binding to osca
namespace osca {

[[noreturn]] auto start() -> void;
[[noreturn]] auto run_core(u32 core_index) -> void;
auto on_keyboard(u8 scancode) -> void;
auto on_timer() -> void;

} // namespace osca

//
// built-in replacements
//

extern "C" auto inline memset(void* s, i32 c, u64 n) -> void* {
    void* original_s = s;
    asm volatile("rep stosb" : "+D"(s), "+c"(n) : "a"(u8(c)) : "memory");
    return original_s;
}

extern "C" auto inline memcpy(void* dest, void const* src, u64 count) -> void* {
    void* original_dest = dest;
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(count) : : "memory");
    return original_dest;
}
