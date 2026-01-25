#pragma once

using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
using uintptr_t = unsigned long long;

typedef struct FrameBuffer {
    uint32_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} FrameBuffer;

extern FrameBuffer frame_buffer;

inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline void serial_print(const char* s) {
    while (*s) {
        outb(0x3F8, *s++);
    }
}

inline void serial_print_hex(uint64_t val) {
    constexpr char hex_chars[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        outb(0x3F8, hex_chars[(val >> i) & 0xF]);
    }
}
