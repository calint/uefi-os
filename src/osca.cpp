#include "osca.hpp"
#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

namespace {

auto draw_rect(u32 const x, u32 const y, u32 const width, u32 const height,
               u32 const color) -> void {

    for (auto i = y; i < y + height; ++i) {
        for (auto j = x; j < x + width; ++j) {
            kernel::frame_buffer.pixels[i * kernel::frame_buffer.stride + j] =
                color;
        }
    }
}

auto draw_char(u32 const col, u32 const row, u32 const color, char c,
               u32 const scale = 1) -> void {

    auto fb = kernel::frame_buffer.pixels;
    auto stride = kernel::frame_buffer.stride;
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
auto print_string(u32 const col, u32 const row, u32 const color,
                  char const* str, u32 const scale = 1) -> void {

    for (auto i = 0u; str[i] != '\0'; ++i) {
        draw_char(col + i, row, color, str[i], scale);
    }
}

auto print_hex(u32 col, u32 const row, u32 const color, u64 const val,
               u32 const scale = 1) -> void {
    char constexpr static hex_chars[]{"0123456789ABCDEF"};
    for (auto i = 60; i >= 0; i -= 4) {
        draw_char(col, row, color, hex_chars[(val >> i) & 0xf], scale);
        col++;
        if (i != 0 && (i % 16) == 0) {
            draw_char(col, row, color, '_', scale);
            col++;
        }
    }
}

auto print_dec(u32 col, u32 const row, u32 const color, u64 val,
               u32 const scale = 1) -> void {
    // case for zero
    if (val == 0) {
        draw_char(col, row, color, '0', scale);
        return;
    }

    // u64 max is 20 digits
    u8 buffer[20];
    auto i = 0u;

    // extract digits in reverse order
    while (val > 0) {
        buffer[i] = u8('0' + (val % 10));
        val /= 10;
        ++i;
    }

    // print the buffer backwards
    while (i > 0) {
        --i;
        draw_char(col, row, color, char(buffer[i]), scale);
        ++col;
    }
}

// prevent optimization so we actually see instructions in the binary
auto __attribute__((noinline)) simd_example(f32* dest, f32 const* src,
                                            const u32 count) -> void {

    for (auto i = 0u; i < count; ++i) {
        // Simple float math: multiply and add
        // This will typically generate MOVSS/ADDSS (scalar) or MOVAPS/ADDPS
        // (packed)
        dest[i] = (src[i] * 1.5f) + 2.0f;
    }
}

// vector type for compiler-assisted vectorization
using v4f __attribute__((vector_size(16))) = float;

// (1) vectorized using compiler extensions
// pointers must be 16-byte aligned, count must be multiple of 4
auto __attribute__((noinline))
simd_example_vectorized(f32* const dest, f32 const* const src, u32 const count)
    -> void {
    for (auto i = 0u; i < count; i += 4) {
        v4f v = *ptr<v4f>(&src[i]);
        v4f r = v * 1.5f + 2.0f;
        *ptr<v4f>(&dest[i]) = r;
    }
}

// (2) sse implementation with memory-based immediate operands
auto __attribute__((noinline)) simd_mul_add_4(f32* const dst,
                                              f32 const* const src) -> void {
    alignas(16) static f32 const mulv[4] = {1.5f, 1.5f, 1.5f, 1.5f};
    alignas(16) static f32 const addv[4] = {2.0f, 2.0f, 2.0f, 2.0f};

    asm volatile(
        "movaps   (%[src]), %%xmm0      \n"
        "mulps    %[mul], %%xmm0        \n"
        "addps    %[add], %%xmm0        \n"
        "movaps   %%xmm0, (%[dst])      \n"
        :
        : [src] "r"(src), [dst] "r"(dst), [mul] "m"(mulv), [add] "m"(addv)
        : "xmm0", "memory");
}

// (3) sse implementation using register-loaded operands
auto __attribute__((noinline)) simd_mul_add_reg(f32* const dst,
                                                f32 const* const src,
                                                f32 const* const mulv,
                                                f32 const* const addv) -> void {
    asm volatile(
        "movaps   (%[src]), %%xmm0  \n"
        "movaps   (%[mul]), %%xmm1  \n"
        "movaps   (%[add]), %%xmm2  \n"
        "mulps    %%xmm1, %%xmm0    \n"
        "addps    %%xmm2, %%xmm0    \n"
        "movaps   %%xmm0, (%[dst])  \n"
        :
        : [src] "r"(src), [dst] "r"(dst), [mul] "r"(mulv), [add] "r"(addv)
        : "xmm0", "xmm1", "xmm2", "memory");
}

// (4) avx implementation (uses ymm registers for 8-wide floats)
auto __attribute__((noinline)) avx_mul_add_8(f32* const dst,
                                             f32 const* const src,
                                             f32 const* const mulv,
                                             f32 const* const addv) -> void {
    asm volatile(
        "vmovaps  (%[src]), %%ymm0  \n"
        "vmovaps  (%[mul]), %%ymm1  \n"
        "vmovaps  (%[add]), %%ymm2  \n"
        "vmulps   %%ymm1, %%ymm0, %%ymm0 \n"
        "vaddps   %%ymm2, %%ymm0, %%ymm0 \n"
        "vmovaps  %%ymm0, (%[dst])  \n"
        :
        : [src] "r"(src), [dst] "r"(dst), [mul] "r"(mulv), [add] "r"(addv)
        : "ymm0", "ymm1", "ymm2", "memory");
}

// triggers red screen panic if calculation is incorrect
auto assert_simd(bool const condition, char const* const msg) -> void {
    if (!condition) {
        kernel::serial::print("simd check failed: ");
        kernel::serial::print(msg);
        kernel::serial::print("\n");
        // trigger panic with a unique color for simd failure (e.g., magenta)
        kernel::panic(0x00ff00ff);
    }
}

auto test_simd_support() -> void {
    alignas(32) f32 src[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    alignas(32) f32 dst[8] = {0};
    alignas(32) f32 mul[8] = {1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f, 1.5f};
    alignas(32) f32 add[8] = {2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f};

    kernel::serial::print("testing sse... ");
    simd_mul_add_4(dst, src);
    assert_simd(dst[0] == 3.5f, "sse immediate");
    kernel::serial::print("ok\n");

    kernel::serial::print("testing sse registers... ");
    simd_mul_add_reg(dst, src, mul, add);
    assert_simd(dst[3] == 8.0f, "sse register"); // 4.0 * 1.5 + 2.0
    kernel::serial::print("ok\n");

    kernel::serial::print("testing vector extensions... ");
    simd_example_vectorized(dst, src, 4);
    assert_simd(dst[1] == 5.0f, "compiler vectorization"); // 2.0 * 1.5 + 2.0
    kernel::serial::print("ok\n");

    kernel::serial::print("testing avx... ");
    avx_mul_add_8(dst, src, mul, add);
    assert_simd(dst[7] == 14.0f, "avx ymm check"); // 8.0 * 1.5 + 2.0
    kernel::serial::print("ok\n");
}

} // namespace

namespace osca {

auto static tick = 0u;

[[noreturn]] auto start() -> void {
    kernel::serial::print("osca x64 kernel is running\n");

    jobs.init();

    auto di = kernel::frame_buffer.pixels;
    for (auto i = 0u;
         i < kernel::frame_buffer.stride * kernel::frame_buffer.height; ++i) {
        *di = 0x00'00'00'22;
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
    auto kernel_addr = u64(kernel::start);
    print_string(col_lbl, row, color, "kernel: ", 3);
    print_hex(col_val, row, color, kernel_addr, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "mmap: ", 3);
    print_hex(col_val, row, color, u64(kernel::memory_map.buffer), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "gfx: ", 3);
    print_hex(col_val, row, color, u64(kernel::frame_buffer.pixels), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "kbd gsi: ", 3);
    print_hex(col_val, row, color, kernel::keyboard_config.gsi, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "kbd flgs: ", 3);
    print_hex(col_val, row, color, kernel::keyboard_config.flags, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "apic io: ", 3);
    print_hex(col_val, row, color, u64(kernel::apic.io), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "lapic: ", 3);
    print_hex(col_val, row, color, u64(kernel::apic.local), 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    auto lapic_id =
        (kernel::apic.local[0x020 / 4] >> 24) & 0xff; // local apic id register
    print_string(col_lbl, row, color, "lapic id: ", 3);
    print_hex(col_val, row, color, lapic_id, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "heapmem: ", 3);
    print_hex(col_val, row, color, kernel::heap.size, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;
    print_string(col_lbl, row, color, "cores: ", 3);
    print_hex(col_val, row, color, kernel::core_count, 3);
    color = color == main_color ? alt_color : main_color;
    ++row;

    test_simd_support();

    kernel::core::interrupts_enable();

    // u32* fb = ptr<u32>(kernel::allocate_pages(
    //     kernel::frame_buffer.height * kernel::frame_buffer.stride / 4096));
    //
    // struct FractalJob {
    //     u32* fb;
    //     u32 width;
    //     u32 height;
    //     u32 stride;
    //     u32 y_start;
    //     u32 y_end;
    //     u32 iteration;
    //     auto run() -> void {
    //         fb += y_start * stride;
    //         for (auto y = y_start; y < y_end; ++y) {
    //             for (auto x = 0u; x < width; ++x) {
    //                 *fb = (y << 8) + x + iteration;
    //                 ++fb;
    //             }
    //             fb += stride - width;
    //         }
    //     }
    // };
    //
    // auto iteration = 0u;
    // auto job_count = kernel::core_count;
    // auto dy = kernel::frame_buffer.height / job_count;
    // auto current_tick = tick;
    // auto frame_no = 0u;
    // auto fps = 0u;
    // while (true) {
    //     auto y = 0u;
    //     for (auto i = 0u; i < job_count; ++i) {
    //         jobs.add<FractalJob>(
    //             fb, kernel::frame_buffer.width, kernel::frame_buffer.height,
    //             kernel::frame_buffer.stride, y, y + dy, iteration);
    //         y += dy;
    //     }
    //     ++iteration;
    //
    //     jobs.wait_idle();
    //
    //     memcpy(kernel::frame_buffer.pixels, fb,
    //            kernel::frame_buffer.height * kernel::frame_buffer.stride *
    //                sizeof(u32));
    //
    //     print_dec(0, 0, 0xff'ff'ff'ff, fps, 3);
    //
    //     ++frame_no;
    //     if (tick - current_tick == 20) {
    //         fps = frame_no / 20;
    //         current_tick = tick;
    //         kernel::serial::print("fps: ");
    //         kernel::serial::print_dec(fps);
    //         kernel::serial::print("\n");
    //     }
    // }

    while (true) {
        kernel::core::pause();
    }
}

auto on_timer() -> void {

    ++tick;

    struct Job {
        u32 color;
        auto run() -> void {
            draw_rect(0, 0, 32, 32, color);
            kernel::serial::print(".");
        }
    };

    jobs.try_add<Job>(tick << 6);
    // note: return ignored, if queue full drop input
}

auto on_keyboard(u8 scancode) -> void {
    auto static kbd_intr_total = 0ull;

    kbd_intr_total++;

    struct Job {
        u64 seq;
        u8 scancode;
        auto run() -> void {
            draw_rect(32, 0, 32, 32, u32(scancode) << 16);
            draw_rect(0, 20 * 8 * 3, kernel::frame_buffer.width, 4 * 8 * 3, 0);
            print_string(1, 20, 0x0000ff00, "kbd intr: ", 3);
            print_hex(12, 20, 0x0000ff00, kbd_intr_total, 3);
            print_string(1, 21, 0x00ffffff, "scancode: ", 3);
            print_hex(12, 21, 0x00ffffff, scancode, 3);
        }
    };

    jobs.try_add<Job>(kbd_intr_total, scancode);
    // note: return ignored, if queue full drop input
}

[[noreturn]] auto run_core([[maybe_unused]] u32 core_id) -> void {
    while (true) {
        if (!jobs.run_next()) {
            // no job was run, queue possibly empty or job not ready. pause
            kernel::core::pause();
        }
    }
}

} // namespace osca
