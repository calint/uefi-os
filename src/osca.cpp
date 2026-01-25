#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

void draw_char(u32 x, u32 y, u32 color, char c) {
    u32* fb = frame_buffer.pixels;
    u32 const stride = frame_buffer.stride;
    if (c < 32 || c > 126) {
        c = '?'; // fallback
    }
    u8 const* glyph = ASCII_FONT[u8(c)];
    for (u8 i = 0; i < 8; ++i) {
        for (u8 j = 0; j < 8; ++j) {
            if (glyph[i] & (1 << (7 - j))) {
                fb[(y + i) * stride + (x + j)] = color;
            }
        }
    }
}

void print_string(u32 x, u32 y, u32 color, char const* str) {
    for (u32 i = 0; str[i] != '\0'; ++i) {
        draw_char(x + (i * 8), y, color, str[i]);
    }
}

[[noreturn]] auto osca() -> void {
    serial_print("osca x64 kernel is running\r\n");
    serial_print("heap size: 0x");
    serial_print_hex(heap.size);
    serial_print("\r\n\r\n");

    u32* di = frame_buffer.pixels;
    for (u32 i = 0; i < frame_buffer.stride * frame_buffer.height; ++i) {
        *di = 0x00000022;
        ++di;
    }
    print_string(20, 20, 0x00FFFF00, "OSCA x64");
    while (true) {
        __asm__("hlt");
    }
}

extern "C" auto osca_apic_timer_handler() -> void {
    serial_print(".");

    // acknowledge interrupt
    *reinterpret_cast<volatile u32*>(0xFEE000B0) = 0;
}
