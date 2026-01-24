#include <stdint.h>

struct [[gnu::packed]] GDTDescriptor {
    uint16_t size;
    uint64_t offset;
};

struct [[gnu::packed]] GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
};

struct [[gnu::packed]] GDT {
    GDTEntry null;
    GDTEntry code;
    GDTEntry data;
};

GDT g_gdt{.null = {0, 0, 0, 0, 0, 0},
          .code = {0, 0, 0, 0x9A, 0x20, 0},
          .data = {0, 0, 0, 0x92, 0x00, 0}};

extern "C" {

auto load_gdt(GDTDescriptor* descriptor) -> void;

alignas(16) uint8_t kernel_stack[16384];

auto load_gdt(GDTDescriptor* descriptor) -> void;

auto switch_stack(uintptr_t stack_top, void (*target)()) -> void;

auto kernel_core() -> void {
    while (true) {
        __asm__("hlt");
    }
}

auto kernel_main(uint32_t* fb, uint32_t stride) -> void {
    // 1. load gdt
    GDTDescriptor gdt_desc{.size = sizeof(GDT) - 1,
                           .offset = reinterpret_cast<uint64_t>(&g_gdt)};
    load_gdt(&gdt_desc);
    for (uint32_t i{0}; i < 100; ++i)
        fb[i] = 0x0000FFFF;
    uintptr_t stack_top{reinterpret_cast<uintptr_t>(kernel_stack) +
                        sizeof(kernel_stack)};
    switch_stack(stack_top, kernel_core);
}
}
