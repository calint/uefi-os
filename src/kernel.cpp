#include <efi.h>

#include "efidef.h"
#include "kernel.hpp"

// note: stack must be 16 byte aligned and top of stack sets RSP
//       make sure top of stack is 16 bytes aligned
alignas(16) static u8 stack[16384 * 16];

FrameBuffer frame_buffer;
MemoryMap memory_map;
KeyboardConfig keyboard_config;
APIC apic;
Heap heap;

namespace {

[[noreturn]] auto panic(u32 color) -> void {
    for (auto i = 0u; i < frame_buffer.stride * frame_buffer.height; ++i) {
        frame_buffer.pixels[i] = color;
    }
    // infinite loop so the hardware doesn't reboot
    while (true) {
        asm("hlt");
    }
}

auto init_sse() -> void {
    u64 cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ull << 2); // clear em (emulation)
    cr0 |= (1ull << 1);  // set mp (monitor coprocessor)
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    u64 cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9);  // set osfxsr (fxsave/fxrstor support)
    cr4 |= (1ull << 10); // set osxmmexcpt (simd exception support)
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    asm volatile("fninit");

    // load a standard mxcsr state
    // bits  7-12: mask all exceptions
    // bits 13-14: round to nearest
    // bit     15: flush to zero
    u32 mxcsr = 0x1f00 | (1 << 15u);
    asm volatile("ldmxcsr %0" ::"m"(mxcsr));
}

auto init_pat() -> void {
    u32 low, high;
    // read the current pat msr (0x277)
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277));

    // the pat is an array of eight 8-bit records.
    // set pa4 (bits 32-34 in the 64-bit msr, or bits 0-2 in 'high') to 0x01,
    // which is the value for write-combining
    high = (high & ~0x07u) | 0x01u;

    // write back the modified pat
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(0x277));

    // serialize
    asm volatile("wbinvd" ::: "memory");
}

auto init_gdt() -> void {
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

    alignas(8) auto static gdt = GDT{.null = {0, 0, 0, 0, 0, 0},
                                     .code = {0, 0, 0, 0x9a, 0x20, 0},
                                     .data = {0, 0, 0, 0x92, 0x00, 0}};

    struct [[gnu::packed]] GDTDescriptor {
        u16 size;
        u64 offset;
    };

    auto descriptor =
        GDTDescriptor{.size = sizeof(GDT) - 1, .offset = u64(&gdt)};

    asm volatile("lgdt %0\n\t"
                 "mov $0x10, %%ax\n\t"
                 "mov %%ax, %%ds\n\t"
                 "mov %%ax, %%es\n\t"
                 "mov %%ax, %%ss\n\t"
                 "pushq $0x08\n\t"
                 "lea 1f(%%rip), %%rax\n\t"
                 "pushq %%rax\n\t"
                 "lretq\n\t"
                 "1:\n\t"
                 :
                 : "m"(descriptor)
                 : "rax", "memory");
}

auto make_heap() -> Heap {
    // find largest contiguous chunk of memory
    auto largest_chunk_size = 0ull;
    auto aligned_start = 0ull;
    auto aligned_size = 0ull;
    auto desc = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map.buffer);
    auto num_descriptors = memory_map.size / memory_map.descriptor_size;
    for (auto i = 0u; i < num_descriptors; ++i) {
        auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(
            u64(desc) + (i * memory_map.descriptor_size));

        if (d->Type == EfiConventionalMemory) {
            auto chunk_start = d->PhysicalStart;
            auto chunk_size = d->NumberOfPages * 4096;
            if (chunk_size > largest_chunk_size) {
                largest_chunk_size = chunk_size;
                aligned_start = (chunk_start + 4095) & ~4095ull;
                aligned_size = (chunk_size + 4095) & ~4095ull;
            }
        }
    }

    return {reinterpret_cast<void*>(aligned_start), aligned_size};
}

auto allocate_page() -> void* {
    if (heap.size < 4096) {
        serial_print("error: out of memory for paging\n");
        panic(0xff00'0000);
    }
    auto ptr = heap.start;
    heap.start = reinterpret_cast<void*>(u64(heap.start) + 4096);
    heap.size -= 4096;
    memset(ptr, 0, 4096);
    return ptr;
}

auto get_next_table(u64* table, u64 index) -> u64* {
    if (!(table[index] & 0x01)) {
        // entry is not present
        auto next = allocate_page();
        // set present (0x01) and writable (0x02)
        table[index] = u64(next) | 0x03;
    }
    // return the pointer by masking out the attribute bits
    return reinterpret_cast<u64*>(table[index] & ~0xfffull);
}

// the top-level PML4 (512GB/entry) potentially covering 256 TB
alignas(4096) u64 boot_pml4[512];

auto map_range(u64 phys, u64 size, u64 flags) -> bool {
    // align to 2MB
    auto start = phys & ~0x1f'ffffull;
    auto end = (phys + size + 0x1f'ffffull) & ~0x1f'ffffull;

    serial_print("map_range: ");
    serial_print_hex(start);
    serial_print(" -> ");
    serial_print_hex(end);
    serial_print("\n");

    // map in 2MB chunks
    for (auto addr = start; addr < end; addr += 0x20'0000) {
        auto pml4_idx = (addr >> 39) & 0x1ff;
        auto pdp_idx = (addr >> 30) & 0x1ff;
        auto pd_idx = (addr >> 21) & 0x1ff;

        auto pdp = get_next_table(boot_pml4, pml4_idx);
        auto pd = get_next_table(pdp, pdp_idx);

        // 0x80 is the "huge page" bit for 2mb entries in the page directory
        pd[pd_idx] = addr | flags | 0x80;
    }
    return true;
}

auto init_paging() -> void {
    // save heap start before allocating pages
    auto heap_start = u64(heap.start);
    auto heap_size = heap.size;

    auto constexpr MMIO_FLAGS = 0x13;
    // 0x01 present | 0x02 writable | 0x10 pcd (uncached)

    // map uefi allocated memory
    auto desc = static_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map.buffer);
    auto num_descriptors = memory_map.size / memory_map.descriptor_size;
    for (auto i = 0u; i < num_descriptors; ++i) {
        auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(
            u64(desc) + (i * memory_map.descriptor_size));

        if ((d->Type == EfiACPIReclaimMemory) ||
            (d->Type == EfiACPIMemoryNVS)) {
            serial_print("* acpi tables\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, 3);
        } else if ((d->Type == EfiLoaderCode) || (d->Type == EfiLoaderData) ||
                   (d->Type == EfiBootServicesCode) ||
                   (d->Type == EfiBootServicesData)) {
            // note: the kernel is loaded by uefi in EfiLoaderCode and Data
            // note: EfiBootServiceCode and Data is mapped because before osca
            //       is started the stack is there
            serial_print("* loaded kernel and current stack\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, 3);
        } else if (d->Type == EfiConventionalMemory) {
            serial_print("* memory\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, 3);
        } else if (d->Type == EfiMemoryMappedIO) {
            serial_print("* mmio region\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, MMIO_FLAGS);
        }
    }

    serial_print("* apic\n");
    map_range(u64(apic.io), 0x1000, MMIO_FLAGS);
    map_range(u64(apic.local), 0x1000, MMIO_FLAGS);

    serial_print("* frame buffer\n");
    map_range(u64(frame_buffer.pixels),
              frame_buffer.stride * frame_buffer.height * 4, 0x101b);
    // flags:
    // 0x01 (present) | 0x02 (rw) | 0x08 (pwt) | 0x10 (pcd) | 0x80 (huge) |
    // 0x1000 (pat bit for 2mb) this points to pat entry 4 (wc) 0x01 (present) |

    serial_print("* heap\n");
    map_range(heap_start, heap_size, 3);

    // load CR3 to activate the dynamic tables
    asm volatile("mov %0, %%cr3" : : "r"(boot_pml4) : "memory");
}

// LAPIC timer runs at the speed of the CPU bus or a crystal oscillator, which
// varies between machines
auto calibrate_apic(u32 hz) -> u32 {
    // tell PIT to wait ~10ms
    // frequency is 1193182 hz. 10ms = 11931 ticks
    outb(0x43, 0x30); // channel 0, lo/hi, mode 0
    outb(0x40, 0x2b); // lo byte of 11931
    outb(0x40, 0x2e); // hi byte of 11931

    // start lapic timer
    apic.local[0x380 / 4] = 0xffff'ffff; // max count

    // wait for pit to finish
    auto status = 0;
    while (!(status & 0x80)) {
        outb(0x43, 0xe2); // read back command
        status = inb(0x40);
    }

    auto ticks_per_10ms =
        0xffff'ffff - apic.local[0x390 / 4]; // current count reg

    return ticks_per_10ms * 100 / hz;
}

auto init_apic_timer() -> void {
    // disable legacy pic
    outb(0x21, 0xff);
    outb(0xa1, 0xff);

    apic.local[0x0f0 / 4] = 0x1ff;          // software enable + spurious vector
    apic.local[0x3e0 / 4] = 0x03;           // divide by 16
    apic.local[0x320 / 4] = (1 << 17) | 32; // periodic mode + vector 32
    apic.local[0x380 / 4] = calibrate_apic(2); // initial count
}

auto io_apic_write(u32 reg, u32 val) -> void {
    apic.io[0] = reg; // select register
    apic.io[4] = val; // write value
}

auto init_keyboard() -> void {
    auto cpu_id = (apic.local[0x020 / 4] >> 24) & 0xff;

    io_apic_write(0x10 + keyboard_config.gsi * 2, 33 | keyboard_config.flags);
    io_apic_write(0x10 + keyboard_config.gsi * 2 + 1, cpu_id << 24);

    // flush with timeout guard
    auto flush_count = 0u;
    while (inb(0x64) & 0x01) {
        inb(0x60);
        if (++flush_count > 100) {
            serial_print("kbd: flush timeout\n");
            break;
        }
    }

    // wait for ready with timeout
    auto wait_count = 0u;
    while (inb(0x64) & 0x02) {
        if (++wait_count > 100000) {
            serial_print("kbd: controller timeout\n");
            return;
        }
        asm volatile("pause");
    }

    // send "enable scanning" to keyboard
    outb(0x60, 0xf4);

    // wait for ack with diagnostic
    auto ack_received = false;
    for (auto i = 0u; i < 1000000; ++i) {
        if (inb(0x64) & 0x01) {
            auto response = inb(0x60);
            if (response == 0xfa) {
                serial_print("kbd: ack\n");
                ack_received = true;
                break;
            }
        }
        asm volatile("pause");
    }

    if (!ack_received) {
        serial_print("kbd: no ack\n");
    }
}

// callback assembler functions
extern "C" auto kernel_asm_timer_handler() -> void;
extern "C" auto kernel_asm_keyboard_handler() -> void;

auto init_idt() -> void {
    struct [[gnu::packed]] IDTEntry {
        u16 low;
        u16 sel;
        u8 ist;
        u8 attr;
        u16 mid;
        u32 high;
        u32 res;
    };

    alignas(16) static IDTEntry idt[256];

    // set idt entry 32 (timer)
    auto apic_addr = u64(kernel_asm_timer_handler);
    idt[32] = {u16(apic_addr),       8, 0, 0x8e, u16(apic_addr >> 16),
               u32(apic_addr >> 32), 0};

    // set idt entry 33 (keyboard)
    auto kbd_addr = u64(kernel_asm_keyboard_handler);
    idt[33] = {u16(kbd_addr),       8, 0, 0x8e, u16(kbd_addr >> 16),
               u32(kbd_addr >> 32), 0};

    struct [[gnu::packed]] IDTR {
        u16 limit;
        u64 base;
    };

    auto idtr = IDTR{sizeof(idt) - 1, u64(idt)};
    asm volatile("lidt %0" : : "m"(idtr));
}

extern "C" auto osca_on_keyboard(u8 scancode) -> void;
extern "C" auto kernel_on_keyboard() -> void {
    // read all pending scancodes from the controller
    while (inb(0x64) & 0x01) {
        auto scancode = inb(0x60);
        serial_print("|");
        serial_print_hex_byte(scancode);
        serial_print("|");
        osca_on_keyboard(scancode);
    }

    // set end of interrupt (EOI)
    apic.local[0x0B0 / 4] = 0;
}

extern "C" auto osca_on_timer() -> void;
extern "C" auto kernel_on_timer() -> void {
    osca_on_timer();

    // set end of interrupt (EOI)
    apic.local[0x0B0 / 4] = 0;
}

[[noreturn]] auto osca_start() -> void {
    asm volatile("mov %0, %%rsp\n\t"
                 "mov %0, %%rbp\n\t"
                 "jmp *%1"
                 :
                 : "r"(&stack[sizeof(stack)] - 8), "r"(osca)
                 : "memory");
    // note: why -8
    // The x86-64 System V ABI requires the stack to be 16-byte aligned at the
    // point a call occurs. Since a call pushes an 8-byte return address, the
    // compiler expects the stack to end in 0x8 upon entering a function.

    __builtin_unreachable();
}
} // namespace

extern "C" [[noreturn]] auto kernel_start() -> void {
    heap = make_heap();

    serial_print("enable_sse");
    init_sse();

    serial_print("init_pat\n");
    init_pat();

    serial_print("init_gdt\n");
    init_gdt();

    serial_print("init_paging\n");
    init_paging();

    serial_print("init_idt\n");
    init_idt();

    serial_print("init_timer\n");
    init_apic_timer();

    serial_print("init_keyboard\n");
    init_keyboard();

    serial_print("osca_start\n");
    osca_start();
}
