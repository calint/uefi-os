#pragma once

auto inline cpu_pause() -> void { __builtin_ia32_pause(); }

auto inline cpu_interrupts_enable() -> void { asm volatile("sti"); }

auto inline cpu_interrupts_disable() -> void { asm volatile("cli"); }
