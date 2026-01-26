#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

void draw_char(u32 x, u32 y, u32 color, char c, u32 scale = 1) {
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
                        fb[(y * scale + (i * scale) + sy) * stride +
                           (x * scale + (j * scale) + sx)] = color;
                    }
                }
            }
        }
    }
}

// Update print_string to pass the scale
void print_string(u32 x, u32 y, u32 color, char const* str, u32 scale = 1) {
    for (u32 i = 0; str[i] != '\0'; ++i) {
        draw_char(x + (i * 8), y, color, str[i], scale);
    }
}

auto print_hex(u32 x, u32 y, u32 color, u64 val, u32 scale = 1) -> void {
    constexpr char hex_chars[]{"0123456789ABCDEF"};
    // print 16 characters for a 64-bit hex value
    for (i32 i = 15; i >= 0; --i) {
        char const c{hex_chars[(val >> (i * 4)) & 0xF]};
        draw_char(x + ((15 - u32(i)) * 8), y, color, c, scale);
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
    print_string(20, 20, 0x00FFFF00, "OSCA x64", 3);
    u64 const kernel_addr = u64(kernel_init);
    print_hex(20, 40, 0xFFFFFFFF, kernel_addr, 3);
    print_hex(20, 60, 0xFFFFFFFF, u64(frame_buffer.pixels), 3);
    volatile u32* lapic = reinterpret_cast<u32*>(0xFEE00000);
    u32 const lapic_id =
        (lapic[0x020 / 4] >> 24) & 0xFF; // Local APIC ID Register
    print_string(20, 80, 0x00FF00FF, "LAPIC ID: ", 3);
    print_hex(100, 80, 0x00FF00FF, lapic_id, 3);

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
    for (u32 y = 120 * 4; y < 160 * 4; ++y) {
        for (u32 x = 0; x < frame_buffer.width; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = 0;
        }
    }

    // Draw big "INTR" label and count
    print_string(20, 120, 0x0000FF00, "KBD INTR: ", 4);
    print_hex(100, 120, 0x0000FF00, kbd_intr_total, 4);

    // Draw the latest scancode extra large
    print_string(20, 140, 0x00FFFFFF, "SCAN: ", 4);
    print_hex(60, 140, 0x00FFFFFF, scancode, 4);

    // Keep your original color box but make it bigger
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 32; x < 32 + 32; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = u32(scancode)
                                                               << 16;
        }
    }
}
