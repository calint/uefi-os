#include <efi.h>

#include "ascii_font_8x8.hpp"
#include "kernel.hpp"

extern "C" void* memset(void* s, int c, unsigned long n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

alignas(16) u8 kernel_stack[16384];

MemoryMap memory_map = {};

Heap heap = {};

struct [[gnu::packed]] GDTDescriptor {
    u16 size;
    u64 offset;
};

struct [[gnu::packed]] GDTEntry {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 granularity;
    u8 base_high;
};

struct [[gnu::packed]] GDT {
    GDTEntry null;
    GDTEntry code;
    GDTEntry data;
};

GDT g_gdt{.null = {0, 0, 0, 0, 0, 0},
          .code = {0, 0, 0, 0x9A, 0x20, 0},
          .data = {0, 0, 0, 0x92, 0x00, 0}};

extern "C" auto kernel_load_gdt(GDTDescriptor* descriptor) -> void;

extern "C" auto kernel_switch_stack(uptr stack_top, void (*target)()) -> void;

auto make_heap() -> Heap {
    Heap result{.start = nullptr, .size = 0};

    EFI_MEMORY_DESCRIPTOR* desc =
        reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map.buffer);

    uptr num_descriptors = memory_map.size / memory_map.descriptor_size;

    for (uptr i = 0; i < num_descriptors; ++i) {
        EFI_MEMORY_DESCRIPTOR* d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(
            reinterpret_cast<uptr>(desc) + (i * memory_map.descriptor_size));

        if (d->Type == EfiConventionalMemory) {
            uptr chunk_start = d->PhysicalStart;
            uptr chunk_size = d->NumberOfPages * 4096;
            if (chunk_size > result.size) {
                result.start = reinterpret_cast<void*>(chunk_start);
                result.size = chunk_size;
            }
        }
    }

    return result;
}

extern "C" auto kernel_init(MemoryMap map) -> void {
    memory_map = map;

    heap = make_heap();

    GDTDescriptor gdt_desc{.size = sizeof(GDT) - 1,
                           .offset = reinterpret_cast<u64>(&g_gdt)};

    serial_print("kernel_load_gdt\r\n");
    kernel_load_gdt(&gdt_desc);

    uptr stack_top =
        reinterpret_cast<uptr>(kernel_stack) + sizeof(kernel_stack);

    serial_print("kernel_switch_stack\r\n");
    kernel_switch_stack(stack_top, osca);
}
