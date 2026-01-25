#pragma once

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using uintptr = unsigned long long;

typedef struct FrameBuffer {
    u32* pixels;
    u32 width;
    u32 height;
    u32 stride;
} FrameBuffer;

extern FrameBuffer frame_buffer;

inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline void serial_print(const char* s) {
    while (*s) {
        outb(0x3F8, *s++);
    }
}

inline void serial_print_hex(u64 val) {
    constexpr char hex_chars[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        outb(0x3F8, hex_chars[(val >> i) & 0xF]);
    }
}
