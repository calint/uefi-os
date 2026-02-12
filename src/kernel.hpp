#pragma once

#include "types.hpp"

namespace kernel {

struct FrameBuffer {
    u32* pixels;
    u32 width;
    u32 height;
    u32 stride;
};

FrameBuffer inline frame_buffer;

struct MemoryMap {
    void* buffer;
    u64 size;
    u64 descriptor_size;
    u32 descriptor_version;
};

MemoryMap inline memory_map;

struct KeyboardConfig {
    u32 gsi;
    u32 flags;
};

KeyboardConfig inline keyboard_config;

struct Apic {
    u32 volatile* io;
    u32 volatile* local;
};

Apic inline apic;

struct Core {
    u8 apic_id;
};

Core inline cores[256];
u8 inline core_count;

struct Heap {
    void* start;
    u64 size;
};

Heap inline heap;

auto inline outb(u16 const port, u8 const val) -> void {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

auto inline inb(u16 const port) -> u8 {
    u8 result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

[[noreturn]] auto start() -> void;

} // namespace kernel

namespace kernel::serial {

auto inline print(char const* s) -> void {
    while (*s) {
        outb(0x3f8, u8(*s));
        ++s;
    }
}

auto inline print_hex_byte(u8 const val) -> void {
    u8 constexpr static hex_chars[] = "0123456789ABCDEF";
    outb(0x3f8, hex_chars[val >> 4]);
    outb(0x3f8, hex_chars[val & 0xf]);
}

auto inline print_hex(u64 const val) -> void {
    print_hex_byte(u8(val >> 56));
    print_hex_byte(u8(val >> 48));
    outb(0x3f8, '_');
    print_hex_byte(u8(val >> 40));
    print_hex_byte(u8(val >> 32));
    outb(0x3f8, '_');
    print_hex_byte(u8(val >> 24));
    print_hex_byte(u8(val >> 16));
    outb(0x3f8, '_');
    print_hex_byte(u8(val >> 8));
    print_hex_byte(u8(val));
}

auto inline print_dec(u64 val) -> void {
    // case for zero
    if (val == 0) {
        outb(0x3f8, '0');
        return;
    }

    // u64 max is 20 digits
    u8 buffer[20];
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

} // namespace kernel::serial

namespace kernel::core {

auto constexpr CACHE_LINE_SIZE = 64u;
// note: almost all modern x86_64 processors (intel and amd)

auto inline pause() -> void { __builtin_ia32_pause(); }
auto inline interrupts_enable() -> void { asm volatile("sti"); }
auto inline interrupts_disable() -> void { asm volatile("cli"); }
auto inline halt() -> void { asm volatile("hlt"); }

} // namespace kernel::core

// kernel callback assembler functions
extern "C" auto kernel_asm_timer_handler() -> void;
extern "C" auto kernel_asm_keyboard_handler() -> void;

// binding to osca
namespace osca {

[[noreturn]] auto start() -> void;
[[noreturn]] auto run_core(u32 core_index) -> void;
auto on_keyboard(u8 scancode) -> void;
auto on_timer() -> void;

} // namespace osca

// required by msvc/clang abi when floating-point arithmetic is used
extern "C" i32 _fltused;

//
// built-in replacements
//

extern "C" auto inline memset(void* s, i32 const c, u64 n) -> void* {
    void* original_s = s;
    asm volatile("rep stosb" : "+D"(s), "+c"(n) : "a"(u8(c)) : "memory");
    return original_s;
}

extern "C" auto inline memcpy(void* dest, void const* src, u64 count) -> void* {

    void* original_dest = dest;
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(count) : : "memory");
    return original_dest;
}

// placement new
auto constexpr inline operator new(size_t, void* p) noexcept -> void* {
    return p;
}

// placement delete
auto constexpr inline operator delete(void*, void*) noexcept -> void {}

// global sized deallocation
// note: not constexpr to match the compiler's expected signature
auto inline operator delete(void*, size_t) noexcept -> void {}
