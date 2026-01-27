#pragma once

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8 = char;
using i16 = short;
using i32 = int;
using i64 = long long;

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

extern MemoryMap memory_map;
extern FrameBuffer frame_buffer;
extern KeyboardConfig keyboard_config;
extern Heap heap;

inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline auto inb(u16 port) -> u8 {
    u8 result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

inline void serial_print(char const* s) {
    while (*s) {
        outb(0x3F8, u8(*s++));
    }
}

inline void serial_print_hex(u64 val) {
    constexpr u8 hex_chars[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        outb(0x3f8, hex_chars[(val >> i) & 0xf]);
    }
}

inline void serial_print_hex_byte(u8 val) {
    constexpr u8 hex_chars[] = "0123456789ABCDEF";
    for (int i = 4; i >= 0; i -= 4) {
        outb(0x3F8, hex_chars[(val >> i) & 0xF]);
    }
}

extern "C" [[noreturn]] auto kernel_init(FrameBuffer, MemoryMap, KeyboardConfig)
    -> void;

[[noreturn]] auto osca() -> void;
