#include "kernel.hpp"
#include "ascii_font_8x8.hpp"

alignas(16) uint8_t kernel_stack[16384];

struct [[gnu::packed]] GDTDescriptor {
    uint16_t size;
    uint64_t offset;
};

struct [[gnu::packed]] GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
};

struct [[gnu::packed]] GDT {
    GDTEntry null;
    GDTEntry code;
    GDTEntry data;
};

GDT g_gdt{.null = {0, 0, 0, 0, 0, 0},
          .code = {0, 0, 0, 0x9A, 0x20, 0},
          .data = {0, 0, 0, 0x92, 0x00, 0}};

extern "C" auto load_gdt(GDTDescriptor* descriptor) -> void;

extern "C" auto switch_stack(uintptr_t stack_top, void (*target)()) -> void;

extern "C" auto kernel_init() -> void {
    GDTDescriptor gdt_desc{.size = sizeof(GDT) - 1,
                           .offset = reinterpret_cast<uint64_t>(&g_gdt)};

    serial_print("load gdt\r\n");
    load_gdt(&gdt_desc);

    uintptr_t stack_top{reinterpret_cast<uintptr_t>(kernel_stack) +
                        sizeof(kernel_stack)};

    auto kernel_main() -> void;

    serial_print("switch_stack\r\n");
    switch_stack(stack_top, kernel_main);
}

void draw_char(uint32_t x, uint32_t y, char c, uint32_t color) {
    uint32_t* fb = frame_buffer.pixels;
    uint32_t stride = frame_buffer.stride;
    if (c < 32 || c > 126) {
        c = '?'; // fallback
    }
    const uint8_t* glyph = ASCII_FONT[(uint8_t)c];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (glyph[i] & (1 << (7 - j))) {
                fb[(y + i) * stride + (x + j)] = color;
            }
        }
    }
}

void print_string(uint32_t x, uint32_t y, const char* str, uint32_t color) {
    uint32_t* fb = frame_buffer.pixels;
    uint32_t stride = frame_buffer.stride;
    for (int i = 0; str[i] != '\0'; ++i) {
        draw_char(x + (i * 8), y, str[i], color);
    }
}

void print_string_hex(uint32_t x, uint32_t y, uint64_t val, uint32_t color) {
    uint32_t* fb = frame_buffer.pixels;
    uint32_t stride = frame_buffer.stride;
    char hex_chars[] = "0123456789ABCDEF";
    print_string(x, y, "0x", color);
    for (int i = 0; i < 16; i++) {
        draw_char(x + 16 + (i * 8), y, hex_chars[(val >> (60 - i * 4)) & 0xF],
                  color);
    }
}

auto kernel_main() -> void {
    serial_print("kernel_main\r\n");
    uint32_t* di = frame_buffer.pixels;
    for (uint32_t i = 0; i < frame_buffer.stride * frame_buffer.height; i++) {
        *di++ = 0x00000022;
    }
    print_string(20, 20, "OSCA x64", 0x00FFFF00);
    while (true) {
        __asm__("hlt");
    }
}
