#pragma once

auto constexpr CACHE_LINE_SIZE = 64u;
// note: almost all modern x86_64 processors (intel and amd)

auto constexpr CORE_STACK_SIZE_PAGES = 2 * 1024 * 1024 / 4096u;
