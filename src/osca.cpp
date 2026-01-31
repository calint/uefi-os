#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

namespace {

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

// prevent optimization so we actually see instructions in the binary
auto __attribute__((noinline)) simd_example(float* dest, float* src, int count)
    -> void {
    for (int i = 0; i < count; ++i) {
        // Simple float math: multiply and add
        // This will typically generate MOVSS/ADDSS (scalar) or MOVAPS/ADDPS
        // (packed)
        dest[i] = (src[i] * 1.5f) + 2.0f;
    }
}

auto test_simd_support() -> void {
    serial_print("testing simd... ");

    alignas(16) float input[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    alignas(16) float output[4] = {0.0f};

    // run the math
    simd_example(output, input, 4);

    // validate the result for the first element: (1.0 * 1.5) + 2.0 = 3.5
    // we cast to int for simple printing since we might not have a
    // float-to-string yet
    auto colr = u32(0);
    if (int(output[0]) == 3 && int(output[0] * 10) % 10 == 5) {
        colr = 0x0000ff00;
        serial_print("success (result is 3.5)\n");
    } else {
        colr = 0x00ff0000;
        serial_print("failure (math wrong)\n");
    }
    for (auto y = 400u; y < 420; ++y) {
        for (auto x = 600u; x < 620; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = colr;
        }
    }
}

auto draw_rect(u32 x, u32 y, u32 width, u32 height, u32 color) -> void {
    for (auto i = y; i < y + height; ++i) {
        for (auto j = x; j < x + width; ++j) {
            frame_buffer.pixels[i * frame_buffer.stride + j] = color;
        }
    }
}
} // namespace

namespace osca {
[[noreturn]] auto start() -> void {
    serial_print("osca x64 kernel is running\n");

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
    print_string(col_lbl, row, 0xffffffff, "keyb gsi: ", 3);
    print_hex(col_val, row, 0xffffffff, keyboard_config.gsi, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "keyb flgs: ", 3);
    print_hex(col_val, row, 0xffffffff, keyboard_config.flags, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "io_apic: ", 3);
    print_hex(col_val, row, 0xffffffff, u64(apic.io), 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "lapic: ", 3);
    print_hex(col_val, row, 0xffffffff, u64(apic.local), 3);
    ++row;
    auto lapic_id =
        (apic.local[0x020 / 4] >> 24) & 0xff; // local apic id register
    print_string(col_lbl, row, 0xffffffff, "lapic id: ", 3);
    print_hex(col_val, row, 0xffffffff, lapic_id, 3);
    ++row;
    print_string(col_lbl, row, 0xffffffff, "cores: ", 3);
    print_hex(col_val, row, 0xffffffff, core_count, 3);
    ++row;

    interrupts_enable();

    test_simd_support();

    while (true) {
        asm("hlt");
    }
}

auto on_timer() -> void {
    auto static tick = 0u;

    serial_print(".");

    ++tick;
    for (auto y = 0u; y < 32; ++y) {
        for (auto x = 0u; x < 32; ++x) {
            frame_buffer.pixels[y * frame_buffer.stride + x] = tick << 6;
        }
    }
}

auto on_keyboard(u8 scancode) -> void {
    auto static kbd_intr_total = 0ull;

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

auto run_task(u32 core_index) -> void {
    // draw a unique rectangle for this core
    auto x_pos = core_index * 60;
    auto y_pos = 300u;
    auto color = 0xff00ff00 | (core_index * 0x1234);

    // interrupts_enable();

    while (true) {
        draw_rect(x_pos, y_pos, 50, 50, color);
        ++color;
        // ensure the writes are visible to the gpu
        // mfence forces memory ordering, and wbinvd flushes all caches
        asm volatile("mfence" ::: "memory");
        asm volatile("wbinvd" ::: "memory");
    }
}
} // namespace osca
