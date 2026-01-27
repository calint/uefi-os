#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

void draw_char(u32 col, u32 row, u32 color, char c, u32 scale = 1) {
    u32* fb = frame_buffer.pixels;
    u32 const stride = frame_buffer.stride;
    if (c < 32 || c > 126) {
        c = '?';
    }
    u8 const* glyph = ASCII_FONT[u8(c)];
    for (u8 i = 0; i < 8; ++i) {
        for (u8 j = 0; j < 8; ++j) {
            if (glyph[i] & (1 << (7 - j))) {
                // Draw a scale x scale block of pixels
                for (u32 sy = 0; sy < scale; ++sy) {
                    for (u32 sx = 0; sx < scale; ++sx) {
                        fb[(row * 8 * scale + (i * scale) + sy) * stride +
                           (col * 8 * scale + (j * scale) + sx)] = color;
                    }
                }
            }
        }
    }
}

// Update print_string to pass the scale
void print_string(u32 col, u32 row, u32 color, char const* str, u32 scale = 1) {
    for (u32 i = 0; str[i] != '\0'; ++i) {
        draw_char(col + i, row, color, str[i], scale);
    }
}

auto print_hex(u32 col, u32 row, u32 color, u64 val, u32 scale = 1) -> void {
    constexpr char hex_chars[]{"0123456789ABCDEF"};
    for (i8 i = 60; i >= 0; i -= 4) {
        draw_char(col, row, color, hex_chars[(val >> i) & 0xf], scale);
        col++;
        if (i != 0 && (i % 16) == 0) {
            draw_char(col, row, color, '_', scale);
            col++;
        }
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
    u32 col_lbl = 1;
    u32 col_val = 12;
    u32 row = 2;
    print_string(col_lbl, row, 0x00ffff00, "osca x64", 3);
    ++row;
    u64 const kernel_addr = u64(kernel_start);
    print_string(col_lbl, row, 0xffffffff, "kerneladdr: ", 3);
    print_hex(col_val, row, 0xffffffff, kernel_addr, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "memmapaddr: ", 3);
    print_hex(col_val, row, 0xffffffff, u64(memory_map.buffer), 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "frameaddr: ", 3);
    print_hex(col_val, row, 0xffffffff, u64(frame_buffer.pixels), 3);
    ++row;
    volatile u32* lapic = reinterpret_cast<u32*>(0xfee00000);
    u32 const lapic_id =
        (lapic[0x020 / 4] >> 24) & 0xff; // local apic id register
    print_string(col_lbl, row, 0xffffffff, "lapic id: ", 3);
    print_hex(col_val, row, 0xffffffff, lapic_id, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "keyb gsi: ", 3);
    print_hex(col_val, row, 0xffffffff, keyboard_config.gsi, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "keyb flgs: ", 3);
    print_hex(col_val, row, 0xffffffff, keyboard_config.flags, 3);
    ++row;

    while (true) {
        __asm__("hlt");
    }
}

static u32 tick;

extern "C" auto osca_on_timer() -> void {
    serial_print(".");

    ++tick;
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = tick << 6;
        }
    }
}

static u64 kbd_intr_total = 0;

extern "C" auto osca_on_keyboard(u8 scancode) -> void {
    kbd_intr_total++;

    // clear
    for (u32 y = 20 * 8 * 3; y < 24 * 8 * 3; ++y) {
        for (u32 x = 0; x < frame_buffer.width; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = 0;
        }
    }

    // Draw big "INTR" label and count
    print_string(2, 20, 0x0000FF00, "KBD INTR: ", 3);
    print_hex(16, 20, 0x0000FF00, kbd_intr_total, 3);

    // Draw the latest scancode extra large
    print_string(2, 21, 0x00FFFFFF, "SCAN: ", 3);
    print_hex(16, 21, 0x00FFFFFF, scancode, 3);

    // Keep your original color box but make it bigger
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 32; x < 32 + 32; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = u32(scancode)
                                                               << 16;
        }
    }
}
