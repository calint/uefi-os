#pragma once

namespace config {

// 2MB core stack
auto constexpr CORE_STACK_SIZE_PAGES = 2 * 1024 * 1024 / 4096u;

// timer ticks 2 times a second
auto constexpr TIMER_FREQUENCY_HZ = 2u;

}; // namespace config
