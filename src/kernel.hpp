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

typedef struct FrameBuffer {
    u32* pixels;
    u32 width;
    u32 height;
    u32 stride;
} FrameBuffer;

typedef struct MemoryMap {
    void* buffer;
    u64 size;
    u64 descriptor_size;
    u32 descriptor_version;
} MemoryMap;

typedef struct KeyboardConfig {
    u32 gsi;
    u32 flags;
} KeyboardConfig;

typedef struct Heap {
    void* start;
    u64 size;
} Heap;

typedef struct APIC {
    u32 volatile* io;
    u32 volatile* local;
} APIC;

extern MemoryMap memory_map;
extern FrameBuffer frame_buffer;
extern KeyboardConfig keyboard_config;
extern APIC apic;
extern Heap heap;

auto inline outb(u16 port, u8 val) -> void {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

auto inline inb(u16 port) -> u8 {
    u8 result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

extern "C" auto inline memset(void* s, int c, u64 n) -> void* {
    asm volatile("rep stosb" : "+D"(s), "+c"(n) : "a"(u8(c)) : "memory", "cc");
    return s;
}

auto inline serial_print(char const* s) -> void {
    while (*s) {
        outb(0x3f8, u8(*s++));
    }
}

auto inline serial_print_hex(u64 val) -> void {
    constexpr u8 hex_chars[] = "0123456789ABCDEF";
    for (auto i = 60; i >= 0; i -= 4) {
        outb(0x3f8, hex_chars[(val >> i) & 0xf]);
        if (i != 0 && (i % 16 == 0)) {
            outb(0x3f8, '_');
        }
    }
}

auto inline serial_print_hex_byte(u8 val) -> void {
    constexpr u8 hex_chars[] = "0123456789ABCDEF";
    for (auto i = 4; i >= 0; i -= 4) {
        outb(0x3F8, hex_chars[(val >> i) & 0xF]);
    }
}

auto inline interrupts_enable() -> void { asm volatile("sti"); }

extern "C" [[noreturn]] auto kernel_start() -> void;

[[noreturn]] auto osca() -> void;
