#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

auto draw_char(u32 col, u32 row, u32 color, char c, u32 scale = 1) -> void {
    auto fb = frame_buffer.pixels;
    auto stride = frame_buffer.stride;
    if (c < 32 || c > 126) {
        c = '?';
    }
    auto glyph = ASCII_FONT[u8(c)];
    for (auto i = 0u; i < 8; ++i) {
        for (auto j = 0u; j < 8; ++j) {
            if (glyph[i] & (1 << (7 - j))) {
                // draw a scale x scale block of pixels
                for (auto sy = 0u; sy < scale; ++sy) {
                    for (auto sx = 0u; sx < scale; ++sx) {
                        fb[(row * 8 * scale + (i * scale) + sy) * stride +
                           (col * 8 * scale + (j * scale) + sx)] = color;
                    }
                }
            }
        }
    }
}

// Update print_string to pass the scale
auto print_string(u32 col, u32 row, u32 color, char const* str, u32 scale = 1)
    -> void {
    for (auto i = 0u; str[i] != '\0'; ++i) {
        draw_char(col + i, row, color, str[i], scale);
    }
}

auto print_hex(u32 col, u32 row, u32 color, u64 val, u32 scale = 1) -> void {
    constexpr char hex_chars[]{"0123456789ABCDEF"};
    for (auto i = 60; i >= 0; i -= 4) {
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

    auto di = frame_buffer.pixels;
    for (auto i = 0u; i < frame_buffer.stride * frame_buffer.height; ++i) {
        *di = 0x00000022;
        ++di;
    }
    auto col_lbl = 1u;
    auto col_val = 12u;
    auto row = 2u;
    print_string(col_lbl, row, 0x00ffff00, "osca x64", 3);
    ++row;
    auto kernel_addr = u64(kernel_start);
    print_string(col_lbl, row, 0xffffffff, "kerneladdr: ", 3);
    print_hex(col_val, row, 0xffffffff, kernel_addr, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "memmapaddr: ", 3);
    print_hex(col_val, row, 0xffffffff, u64(memory_map.buffer), 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "frameaddr: ", 3);
    print_hex(col_val, row, 0xffffffff, u64(frame_buffer.pixels), 3);
    ++row;
    volatile auto lapic = reinterpret_cast<u32*>(0xfee00000);
    auto lapic_id = (lapic[0x020 / 4] >> 24) & 0xff; // local apic id register
    print_string(col_lbl, row, 0xffffffff, "lapic id: ", 3);
    print_hex(col_val, row, 0xffffffff, lapic_id, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "keyb gsi: ", 3);
    print_hex(col_val, row, 0xffffffff, keyboard_config.gsi, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "keyb flgs: ", 3);
    print_hex(col_val, row, 0xffffffff, keyboard_config.flags, 3);
    ++row;

    interrupts_enable();

    while (true) {
        asm("hlt");
    }
}

static u32 tick;

extern "C" auto osca_on_timer() -> void {
    serial_print(".");

    ++tick;
    for (auto y = 0u; y < 32; ++y) {
        for (auto x = 0u; x < 32; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = tick << 6;
        }
    }
}

static auto kbd_intr_total = 0ull;

extern "C" auto osca_on_keyboard(u8 scancode) -> void {
    kbd_intr_total++;

    // clear
    for (auto y = 20 * 8 * 3u; y < 24 * 8 * 3; ++y) {
        for (auto x = 0u; x < frame_buffer.width; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = 0;
        }
    }

    // Draw big "INTR" label and count
    print_string(1, 20, 0x0000ff00, "kbd intr: ", 3);
    print_hex(12, 20, 0x0000ff00, kbd_intr_total, 3);

    // draw the latest scancode extra large
    print_string(1, 21, 0x00ffffff, "scancode: ", 3);
    print_hex(12, 21, 0x00ffffff, scancode, 3);

    // Keep your original color box but make it bigger
    for (auto y = 0u; y < 32; ++y) {
        for (auto x = 32u; x < 32 + 32; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = u32(scancode)
                                                               << 16;
        }
    }
}
