#include "osca.hpp"
#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

namespace {

auto draw_rect(u32 x, u32 y, u32 width, u32 height, u32 color) -> void {
    for (auto i = y; i < y + height; ++i) {
        for (auto j = x; j < x + width; ++j) {
            frame_buffer.pixels[i * frame_buffer.stride + j] = color;
        }
    }
}

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
                draw_rect(col * 8 * scale + j * scale,
                          row * 8 * scale + i * scale, scale, scale, color);
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
    serial_print("  test simd: ");

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
        serial_print("ok\n");
    } else {
        colr = 0x00ff0000;
        serial_print("failed\n");
    }
    draw_rect(600u, 400u, 20u, 20u, colr);
}

} // namespace

namespace osca {

alignas(CACHE_LINE_SIZE) Jobs jobs; // note: 0 initialized

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
    auto main_color = 0xff'ff'ff'ffu;
    auto alt_color = 0xc0'c0'c0'c0u;
    auto color = main_color;
    print_string(col_lbl, row, 0x00ffff00, "osca x64", 3);
    ++row;
    auto kernel_addr = u64(kernel_start);
    print_string(col_lbl, row, color, "kernel: ", 3);
    print_hex(col_val, row, color, kernel_addr, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "mmap: ", 3);
    print_hex(col_val, row, color, u64(memory_map.buffer), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "gfx: ", 3);
    print_hex(col_val, row, color, u64(frame_buffer.pixels), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "kbd gsi: ", 3);
    print_hex(col_val, row, color, keyboard_config.gsi, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "kbd flgs: ", 3);
    print_hex(col_val, row, color, keyboard_config.flags, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "apic io: ", 3);
    print_hex(col_val, row, color, u64(apic.io), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "lapic: ", 3);
    print_hex(col_val, row, color, u64(apic.local), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    auto lapic_id =
        (apic.local[0x020 / 4] >> 24) & 0xff; // local apic id register
    print_string(col_lbl, row, color, "lapic id: ", 3);
    print_hex(col_val, row, color, lapic_id, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "cores: ", 3);
    print_hex(col_val, row, color, core_count, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;

    test_simd_support();

    interrupts_enable();

    while (true) {
        asm volatile("hlt");
    }
}

auto on_timer() -> void {
    auto static tick = 0u;

    ++tick;

    struct Job {
        u32 color;
        auto run() -> void {
            draw_rect(0, 0, 32, 32, color);
            serial_print(".");
        }
    };

    jobs.add(Job{tick << 6});
}

auto on_keyboard(u8 scancode) -> void {
    auto static kbd_intr_total = 0ull;

    kbd_intr_total++;

    struct Job {
        u64 seq;
        u8 scancode;
        auto run() -> void {
            draw_rect(32, 0, 32, 32, u32(scancode) << 16);
            draw_rect(0, 20 * 8 * 3, frame_buffer.width, 4 * 8 * 3, 0);
            print_string(1, 20, 0x0000ff00, "kbd intr: ", 3);
            print_hex(12, 20, 0x0000ff00, kbd_intr_total, 3);
            print_string(1, 21, 0x00ffffff, "scancode: ", 3);
            print_hex(12, 21, 0x00ffffff, scancode, 3);
        }
    };

    jobs.try_add(Job{kbd_intr_total, scancode});
    // note: return ignored, if queue full drop input
}

[[noreturn]] auto run_core([[maybe_unused]] u32 core_id) -> void {
    while (true) {
        if (!jobs.run_next()) {
            // queue was for sure empty
            asm volatile("pause");
        }
    }
}

} // namespace osca
