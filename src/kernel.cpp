#include "kernel.hpp"
#include "ascii_font_8x8.hpp"

alignas(16) u8 kernel_stack[16384];

struct [[gnu::packed]] GDTDescriptor {
    u16 size;
    u64 offset;
};

struct [[gnu::packed]] GDTEntry {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 granularity;
    u8 base_high;
};

struct [[gnu::packed]] GDT {
    GDTEntry null;
    GDTEntry code;
    GDTEntry data;
};

GDT g_gdt{.null = {0, 0, 0, 0, 0, 0},
          .code = {0, 0, 0, 0x9A, 0x20, 0},
          .data = {0, 0, 0, 0x92, 0x00, 0}};

extern "C" auto kernel_load_gdt(GDTDescriptor* descriptor) -> void;

extern "C" auto kernel_switch_stack(uintptr stack_top, void (*target)())
    -> void;

extern "C" auto kernel_init() -> void {
    GDTDescriptor gdt_desc{.size = sizeof(GDT) - 1,
                           .offset = reinterpret_cast<u64>(&g_gdt)};

    serial_print("kernel_load_gdt\r\n");
    kernel_load_gdt(&gdt_desc);

    uintptr stack_top =
        reinterpret_cast<uintptr>(kernel_stack) + sizeof(kernel_stack);

    serial_print("kernel_switch_stack\r\n");
    auto kernel_main() -> void;
    serial_print("kernel_main\r\n");
    kernel_switch_stack(stack_top, kernel_main);
}

void draw_char(u32 x, u32 y, u32 color, char c) {
    u32* fb = frame_buffer.pixels;
    u32 const stride = frame_buffer.stride;
    if (c < 32 || c > 126) {
        c = '?'; // fallback
    }
    u8 const* glyph = ASCII_FONT[(u8)c];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (glyph[i] & (1 << (7 - j))) {
                fb[(y + i) * stride + (x + j)] = color;
            }
        }
    }
}

void print_string(u32 x, u32 y, u32 color, char const* str) {
    u32* fb = frame_buffer.pixels;
    u32 stride = frame_buffer.stride;
    for (int i = 0; str[i] != '\0'; ++i) {
        draw_char(x + (i * 8), y, color, str[i]);
    }
}

auto kernel_main() -> void {
    serial_print("osca x64 kernel is running\r\n");
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
