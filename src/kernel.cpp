#include <efi.h>

#include "kernel.hpp"

alignas(16) static u8 kernel_stack[16384];

FrameBuffer frame_buffer;
MemoryMap memory_map;
KeyboardConfig keyboard_config;
Heap heap;

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

alignas(8) static GDT gdt{.null = {0, 0, 0, 0, 0, 0},
                          .code = {0, 0, 0, 0x9A, 0x20, 0},
                          .data = {0, 0, 0, 0x92, 0x00, 0}};

extern "C" auto kernel_load_gdt(GDTDescriptor* descriptor) -> void;

extern "C" [[noreturn]] auto osca_start(u64 stack_top, void (*target)())
    -> void;

extern "C" void* memset(void* s, int c, unsigned long n) {
    unsigned char* p = reinterpret_cast<unsigned char*>(s);
    while (n--) {
        *p++ = static_cast<unsigned char>(c);
    }
    return s;
}

extern "C" void* memcpy(void* dest, const void* src, u64 n) {
    u8* d = reinterpret_cast<u8*>(dest);
    const u8* s = reinterpret_cast<const u8*>(src);

    while (n--) {
        *d++ = *s++;
    }

    return dest;
}

static auto make_heap() -> Heap {
    Heap result{};

    auto desc = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map.buffer);

    u64 num_descriptors = memory_map.size / memory_map.descriptor_size;

    for (u64 i = 0; i < num_descriptors; ++i) {
        auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(
            u64(desc) + (i * memory_map.descriptor_size));

        if (d->Type == EfiConventionalMemory) {
            u64 chunk_start = d->PhysicalStart;
            u64 chunk_size = d->NumberOfPages * 4096;
            if (chunk_size > result.size) {
                u64 aligned_start = (chunk_start + 4095) & ~4095ull;
                result.start = reinterpret_cast<void*>(aligned_start);
                result.size = chunk_size;
            }
        }
    }

    return result;
}

static auto allocate_page() -> void* {
    if (heap.size < 4096) {
        serial_print("error: out of memory for paging\r\n");
        return nullptr;
    }
    void* ptr = heap.start;
    heap.start = reinterpret_cast<void*>(u64(heap.start) + 4096);
    heap.size -= 4096;
    memset(ptr, 0, 4096);
    return ptr;
}

// Only the top-level PML4 remains as a static array
alignas(4096) static u64 boot_pml4[512];

static auto get_next_table(u64* table, u64 index) -> u64* {
    if (!(table[index] & 0x01)) { // If entry is not present
        void* next = allocate_page();
        if (next == nullptr) {
            return nullptr;
        }
        // set present (0x01) and writable (0x02)
        table[index] = reinterpret_cast<u64>(next) | 0x03;
    }
    // Return the pointer by masking out the attribute bits
    return reinterpret_cast<u64*>(table[index] & ~0xfffull);
}

static auto map_range(u64 phys, u64 size, u64 flags) -> void {
    u64 const end = phys + size;
    // map in 2MB chunks
    for (u64 addr = phys; addr < end; addr += 0x20'0000) {
        u64 pml4_idx = (addr >> 39) & 0x1ff;
        u64 pdp_idx = (addr >> 30) & 0x1ff;
        u64 pd_idx = (addr >> 21) & 0x1ff;

        u64* pdp = get_next_table(boot_pml4, pml4_idx);
        if (pdp == nullptr) {
            return;
        }

        u64* pd = get_next_table(pdp, pdp_idx);
        if (pd == nullptr) {
            return;
        }

        // 0x80 is the "Huge Page" bit for 2MB entries in the Page Directory
        pd[pd_idx] = addr | flags | 0x80;
    }
}

static auto init_paging() -> void {
    // identity map the first 8GB
    // This covers the kernel (5GB), heap, and stack
    map_range(0, 0x2'0000'0000ull, 0x03);

    // map the framebuffer dynamically
    u64 const fb_base = u64(frame_buffer.pixels);
    u64 const fb_size = u64(frame_buffer.stride) * frame_buffer.height * 4;

    // align start/end to 2MB boundaries for our huge-page mapper
    u64 const fb_start = fb_base & ~0x1f'ffffull;
    u64 const fb_end = (fb_base + fb_size + 0x1f'ffffull) & ~0x1'fffffull;
    map_range(fb_start, fb_end - fb_start, 0x03);

    // map APIC regions (typically 0xffc0'0000 and 0xfee0'0000)
    // using cache disable (0x10) and write hhrough (0x08)
    map_range(0xfec0'0000, 0x20'0000, 0x1b);
    map_range(0xfee0'0000, 0x20'0000, 0x1b);

    // load CR3 to activate the dynamic tables
    asm volatile("mov %0, %%cr3" : : "r"(boot_pml4) : "memory");
}

struct [[gnu::packed]] IDTEntry {
    u16 low;
    u16 sel;
    u8 ist;
    u8 attr;
    u16 mid;
    u32 high;
    u32 res;
};

struct [[gnu::packed]] IDTR {
    u16 limit;
    u64 base;
};

alignas(16) static IDTEntry idt[256];

static volatile u32* lapic = reinterpret_cast<u32*>(0xfee0'0000);

// LAPIC timer runs at the speed of the CPU bus or a crystal oscillator, which
// varies between machines
auto calibrate_apic(u32 hz) -> u32 {
    // tell PIT to wait ~10ms
    // frequency is 1193182 hz. 10ms = 11931 ticks
    outb(0x43, 0x30); // channel 0, lo/hi, mode 0
    outb(0x40, 0x2b); // lo byte of 11931
    outb(0x40, 0x2e); // hi byte of 11931

    // start lapic timer
    lapic[0x380 / 4] = 0xffff'ffff; // max count

    // wait for pit to finish
    u8 status = 0;
    while (!(status & 0x80)) {
        outb(0x43, 0xe2); // read back command
        status = inb(0x40);
    }

    u32 ticks_per_10ms = 0xffff'ffff - lapic[0x390 / 4]; // current count reg

    return ticks_per_10ms * 100 / hz;
}

static auto init_apic_timer() -> void {
    // disable legacy pic
    outb(0x21, 0xff);
    outb(0xa1, 0xff);

    lapic[0x0f0 / 4] = 0x1ff;             // software enable + spurious vector
    lapic[0x3e0 / 4] = 0x03;              // divide by 16
    lapic[0x320 / 4] = (1 << 17) | 32;    // periodic mode + vector 32
    lapic[0x380 / 4] = calibrate_apic(2); // initial count
}

static auto io_apic_write(u32 reg, u32 val) -> void {
    volatile u32* io_apic = reinterpret_cast<u32*>(0xfec0'0000);
    io_apic[0] = reg; // select register
    io_apic[4] = val; // write value
}

static auto init_io_apic() -> void {
    u32 const cpu_id = (lapic[0x020 / 4] >> 24) & 0xff;

    io_apic_write(0x10 + keyboard_config.gsi * 2, 33 | keyboard_config.flags);
    io_apic_write(0x10 + keyboard_config.gsi * 2 + 1, cpu_id << 24);
}

static auto init_keyboard_hardware() -> void {
    // read all pending input
    while (inb(0x64) & 0x01) {
        inb(0x60);
    }

    // wait for input buffer empty before sending "enable scanning" command
    while (inb(0x64) & 0x02)
        ;

    // send "enable scanning" command to the keyboard
    outb(0x60, 0xf4);

    // wait for ACK (0xfa)
    for (u32 i = 0; i < 1'000'000; ++i) { // larger timeout for slow hardware
        // check if there is data to read
        if (inb(0x64) & 0x01) {
            if (inb(0x60) == 0xfa) {
                break;
            }
        }
        // small delay to prevent hammering the bus too hard
        __asm__ volatile("pause");
    }
}

// assembler functions
extern "C" auto kernel_asm_timer_handler() -> void;
extern "C" auto kernel_asm_keyboard_handler() -> void;

static auto init_idt() -> void {
    // set idt entry 32 (timer)
    u64 apic_addr = u64(kernel_asm_timer_handler);
    idt[32] = {u16(apic_addr),       8, 0, 0x8e, u16(apic_addr >> 16),
               u32(apic_addr >> 32), 0};

    // set idt entry 33 (keyboard)
    u64 kbd_addr = u64(kernel_asm_keyboard_handler);
    idt[33] = {u16(kbd_addr),       0x08, 0, 0x8e, u16(kbd_addr >> 16),
               u32(kbd_addr >> 32), 0};

    IDTR idtr = {sizeof(idt) - 1, u64(idt)};
    asm volatile("lidt %0" : : "m"(idtr));
}

extern "C" auto osca_on_keyboard(u8 scancode) -> void;

extern "C" auto kernel_on_keyboard() -> void {
    // read all pending scancodes from the controller
    while (inb(0x64) & 0x01) {
        u8 scancode = inb(0x60);
        serial_print("|");
        serial_print_hex_byte(scancode);
        serial_print("|");
        osca_on_keyboard(scancode);
    }

    // set end of interrupt (EOI)
    lapic[0x0B0 / 4] = 0;
}

extern "C" auto osca_on_timer() -> void;

extern "C" auto kernel_on_timer() -> void {
    osca_on_timer();

    // set end of interrupt (EOI)
    lapic[0x0B0 / 4] = 0;
}

extern "C" [[noreturn]] auto kernel_start() -> void {

    heap = make_heap();

    serial_print("kernel_load_gdt\r\n");
    GDTDescriptor gdt_desc{.size = sizeof(GDT) - 1, .offset = u64(&gdt)};
    kernel_load_gdt(&gdt_desc);

    serial_print("init_paging\r\n");
    init_paging();

    serial_print("init_idt\r\n");
    init_idt();

    serial_print("init_apic_timer\r\n");
    init_apic_timer();

    serial_print("init_io_apic\r\n");
    init_io_apic();

    serial_print("init_keyboard_hardware\r\n");
    init_keyboard_hardware();

    asm volatile("sti");

    serial_print("osca_start\r\n");
    u64 stack_top = u64(kernel_stack) + sizeof(kernel_stack);
    osca_start(stack_top, osca);
}
