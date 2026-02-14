#include "osca.hpp"
#include "ascii_font_8x8.hpp"
#include "config.hpp"
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

class Printer {
    kernel::FrameBuffer fb_;
    u32 row_ = 0;
    u32 col_ = 0;
    u32 color_ = 0xff'ff'ff'ff;
    u32 scale_ = 1;
    u32 defcol_ = 0;

    auto drwchr(char c) -> void {

        auto fb = fb_.pixels;
        auto stride = fb_.stride;
        if (c < 32 || c > 126) {
            c = '?';
        }
        auto glyph = ASCII_FONT[u8(c)];
        for (auto i = 0u; i < 8; ++i) {
            for (auto j = 0u; j < 8; ++j) {
                if (glyph[i] & (1 << (7 - j))) {
                    drwrct(col_ * 8 * scale_ + j * scale_,
                           row_ * 8 * scale_ + i * scale_, scale_, scale_,
                           color_);
                }
            }
        }
    }

    auto drwrct(u32 const x, u32 const y, u32 const width, u32 const height,
                u32 const color) -> void {

        for (auto i = y; i < y + height; ++i) {
            for (auto j = x; j < x + width; ++j) {
                fb_.pixels[i * kernel::frame_buffer.stride + j] = color;
            }
        }
    }

  public:
    Printer(kernel::FrameBuffer fb) : fb_{fb} {}

    auto position(u32 const col, u32 const row) -> Printer& {
        row_ = row;
        defcol_ = col_ = col;
        return *this;
    }

    auto color(u32 const color) -> Printer& {
        color_ = color;
        return *this;
    }

    auto color() const -> u32 { return color_; }

    auto scale(u32 const s) -> Printer& {
        scale_ = s;
        return *this;
    }

    auto p(char const* str) -> Printer& {
        while (*str) {
            drwchr(*str);
            ++str;
            ++col_;
        }
        return *this;
    }

    auto p(u64 val) -> Printer& {
        // case for zero
        if (val == 0) {
            drwchr('0');
            return *this;
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
            drwchr(char(buffer[i]));
            ++col_;
        }
        return *this;
    }

    auto p_hex(u64 const val) -> Printer& {
        char constexpr static hex_chars[]{"0123456789ABCDEF"};
        for (auto i = 60; i >= 0; i -= 4) {
            drwchr(hex_chars[(val >> i) & 0xf]);
            ++col_;
            if (i != 0 && (i % 16) == 0) {
                drwchr('_');
                ++col_;
            }
        }
        return *this;
    }

    auto nl() -> Printer& {
        col_ = defcol_;
        ++row_;
        return *this;
    }
};

auto static tick = 0u;

[[noreturn]] auto start() -> void {
    kernel::serial::print("osca x64 kernel is running\n");

    jobs.init();

    // auto di = kernel::frame_buffer.pixels;
    // for (auto i = 0u;
    //      i < kernel::frame_buffer.stride * kernel::frame_buffer.height; ++i)
    //      {
    //     *di = 0x00'00'00'22;
    //     ++di;
    // }
    //
    // auto main_color = 0xff'ff'ff'ffu;
    // auto alt_color = 0xc0'c0'c0'c0u;
    // auto color = main_color;
    //
    // Printer pr = {kernel::frame_buffer};
    //
    // pr.scale(3).color(0x00'ff'ff'00).position(1u, 2u);
    // pr.p("osca x64").nl();
    // pr.color(main_color);
    //
    // pr.p("         kernel: ").p_hex(u64(kernel::start)).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("     memory map: ").p_hex(u64(kernel::memory_map.buffer)).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("   frame buffer: ").p_hex(u64(kernel::frame_buffer.pixels)).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("   keyboard gsi: ").p(kernel::keyboard_config.gsi).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p(" keyboard flags: ").p_hex(kernel::keyboard_config.flags).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("        apic io: ").p_hex(u64(kernel::apic.io)).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("     apic local: ").p_hex(u64(kernel::apic.local)).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("         cpu id: ").p(kernel::apic.local[0x020 / 4] >> 24).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("      heap size: ").p_hex(kernel::heap.size).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // pr.p("          cores: ").p(kernel::core_count).nl();
    // pr.color(pr.color() == main_color ? alt_color : main_color);
    //
    // test_simd_support();
    //
    // kernel::core::interrupts_enable();
    //
    // while (true) {
    //     kernel::core::pause();
    // }

    auto const frame_buffer_pages_count =
        (kernel::frame_buffer.height * kernel::frame_buffer.stride + 4095) /
        4096;
    u32* pixels = ptr<u32>(kernel::allocate_pages(frame_buffer_pages_count));

    kernel::FrameBuffer fb = kernel::frame_buffer;
    fb.pixels = pixels;

    Printer pr = {fb};
    pr.scale(3);

    struct FractalJob {
        u32* pixels;
        u32 width;
        u32 height;
        u32 stride;
        u32 y_start;
        u32 y_end;
        u32 frame; // use frame for zoom level

        auto run() -> void {
            // Target coordinates to zoom into
            auto const target_re = -0.743643f;
            auto const target_im = 0.131825f;

            // Calculate zoom scale: shrinks as frame increases
            auto zoom = 1.0f;
            for (auto i = 0u; i < (frame % 500u); ++i) {
                zoom *= 0.95f;
            }

            // Define the viewport based on the current zoom
            auto const base_w = 3.5f;
            auto const base_h = 2.0f;
            auto const min_re = target_re - (base_w * zoom) / 2.0f;
            auto const max_re = target_re + (base_w * zoom) / 2.0f;
            auto const min_im = target_im - (base_h * zoom) / 2.0f;
            auto const max_im = target_im + (base_h * zoom) / 2.0f;

            auto const re_factor = (max_re - min_re) / float(width - 1u);
            auto const im_factor = (max_im - min_im) / float(height - 1u);

            for (auto y = y_start; y < y_end; ++y) {
                auto c_im = max_im - float(y) * im_factor;
                for (auto x = 0u; x < width; ++x) {
                    auto c_re = min_re + float(x) * re_factor;

                    auto z_re = c_re, z_im = c_im;
                    auto iteration = 0u;
                    // increase max iterations as you zoom for better detail
                    auto const max_iterations = 128u;

                    while ((z_re * z_re + z_im * z_im <= 4.0f) &&
                           (iteration < max_iterations)) {
                        auto next_re = z_re * z_re - z_im * z_im + c_re;
                        auto next_im = 2.0f * z_re * z_im + c_im;
                        z_re = next_re;
                        z_im = next_im;
                        ++iteration;
                    }

                    auto color = 0u;
                    if (iteration < max_iterations) {
                        // dynamic coloring: blue shifts based on zoom/frame
                        auto blue = (iteration * 255u / max_iterations) & 0xffu;
                        auto red = (frame / 2u) & 0xffu;
                        color = (red << 16u) | (blue << 8u) | 255u;
                    } else {
                        color = 0x00000000;
                    }

                    pixels[y * stride + x] = color;
                }
            }
        }
    };

    auto job_count = 1u;
    auto fps_tick = tick;
    auto fps_frame = 0u;
    auto fps = 0u;
    auto fractal_zoom = 0u;

    kernel::core::interrupts_enable();

    while (true) {
        auto dy = kernel::frame_buffer.height / job_count;
        auto y = 0u;
        for (auto i = 0u; i < job_count; ++i) {
            // if height isn't perfectly divisible, the last core takes the
            // remainder
            auto y_end =
                (i == job_count - 1) ? kernel::frame_buffer.height : y + dy;

            jobs.add<FractalJob>(
                pixels, kernel::frame_buffer.width, kernel::frame_buffer.height,
                kernel::frame_buffer.stride, y, y_end, fractal_zoom);

            y = y_end;
        }

        jobs.wait_idle();

        pr.position(1, 1);
        pr.p("cores: ")
            .p(kernel::core_count)
            .p("   jobs: ")
            .p(job_count)
            .p("   fps: ")
            .p(fps);

        memcpy(kernel::frame_buffer.pixels, pixels,
               kernel::frame_buffer.height * kernel::frame_buffer.stride *
                   sizeof(u32));

        ++fps_frame;
        //++fractal_zoom;

        auto const dt = tick - fps_tick;
        auto constexpr static seconds_per_fps_calculation = 10;
        if (dt >= config::TIMER_FREQUENCY_HZ * seconds_per_fps_calculation) {
            fps = fps_frame * config::TIMER_FREQUENCY_HZ / dt;
            fps_frame = 0;
            fps_tick = tick;
            job_count = (job_count % 32) + 1;
            kernel::serial::print("fps: ");
            kernel::serial::print_dec(fps);
            kernel::serial::print("\n");
        }
    }
}

auto on_timer() -> void {
    ++tick;

    draw_rect(0, 0, 32, 32, tick << 6);
}

auto static kbd_intr_total = 0ull;
auto on_keyboard(u8 const scancode) -> void {
    kbd_intr_total++;

    draw_rect(32, 0, 32, 32, u32(scancode) << 16);
    draw_rect(0, 20 * 8 * 3, kernel::frame_buffer.width, 4 * 8 * 3, 0);
    print_string(1, 20, 0x0000ff00, "kbd intr: ", 3);
    print_hex(12, 20, 0x0000ff00, kbd_intr_total, 3);
    print_string(1, 21, 0x00ffffff, "scancode: ", 3);
    print_hex(12, 21, 0x00ffffff, scancode, 3);
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
