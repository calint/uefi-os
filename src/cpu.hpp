#pragma once

namespace cpu {

auto inline pause() -> void { __builtin_ia32_pause(); }
auto inline interrupts_enable() -> void { asm volatile("sti"); }
auto inline interrupts_disable() -> void { asm volatile("cli"); }
auto inline halt() -> void { asm volatile("hlt"); }

} // namespace cpu
