#pragma once

namespace cpu {

auto constexpr CACHE_LINE_SIZE = 64u;
// note: almost all modern x86_64 processors (intel and amd)

auto inline pause() -> void { __builtin_ia32_pause(); }
auto inline interrupts_enable() -> void { asm volatile("sti"); }
auto inline interrupts_disable() -> void { asm volatile("cli"); }
auto inline halt() -> void { asm volatile("hlt"); }

} // namespace cpu
