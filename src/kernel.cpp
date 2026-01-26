#include <efi.h>

#include "kernel.hpp"

alignas(16) static u8 kernel_stack[16384];

FrameBuffer frame_buffer;
MemoryMap memory_map;
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

static auto get_next_table(u64& entry) -> u64* {
    if (!(entry & 1)) {
        void* next = allocate_page();
        entry = u64(next) | 0x03; // present + writable
    }
    return reinterpret_cast<u64*>(entry & ~0xfffu);
}

// Only the top-level PML4 remains as a static array
alignas(4096) static u64 boot_pml4[512];

// Fixed: Corrected parameter type to u64* and added proper indexing
static auto get_next_table(u64* table, u64 index) -> u64* {
    if (!(table[index] & 0x01)) { // If entry is not present
        void* next = allocate_page();
        if (next == nullptr)
            return nullptr;

        // Zero out the new table to prevent garbage mappings
        memset(next, 0, 4096);
        // Set Present (0x01) and Writable (0x02)
        table[index] = reinterpret_cast<u64>(next) | 0x03;
    }
    // Return the pointer by masking out the attribute bits
    return reinterpret_cast<u64*>(table[index] & ~0xFFFull);
}

static auto map_range(u64 phys, u64 size, u64 flags) -> void {
    u64 const end = phys + size;
    // Map in 2MB chunks
    for (u64 addr = phys; addr < end; addr += 0x200000) {
        u64 pml4_idx = (addr >> 39) & 0x1FF;
        u64 pdp_idx = (addr >> 30) & 0x1FF;
        u64 pd_idx = (addr >> 21) & 0x1FF; // Declared here

        u64* pdp = get_next_table(boot_pml4, pml4_idx);
        if (pdp == nullptr)
            return;

        u64* pd = get_next_table(pdp, pdp_idx);
        if (pd == nullptr)
            return;

        // 0x80 is the "Huge Page" bit for 2MB entries in the Page Directory
        pd[pd_idx] = addr | flags | 0x80;
    }
}

static auto init_paging() -> void {
    // Clear the static PML4
    memset(boot_pml4, 0, 4096);

    // 1. Identity map the first 8GB
    // This covers the kernel (5GB), heap, and stack
    map_range(0, 0x200000000ull, 0x03);

    // 2. Map the Framebuffer dynamically
    u64 const fb_base = reinterpret_cast<u64>(frame_buffer.pixels);
    u64 const fb_size =
        static_cast<u64>(frame_buffer.stride) * frame_buffer.height * 4;

    // Align start/end to 2MB boundaries for our huge-page mapper
    u64 const fb_start = fb_base & ~0x1FFFFFull;
    u64 const fb_end = (fb_base + fb_size + 0x1FFFFFull) & ~0x1FFFFFull;
    map_range(fb_start, fb_end - fb_start, 0x03);

    // 3. Map APIC regions (typically 0xFEC00000 and 0xFEE00000)
    // Using Cache Disable (0x10) and Write Through (0x08)
    map_range(0xFEC00000, 0x200000, 0x1B);
    map_range(0xFEE00000, 0x200000, 0x1B);

    // Load CR3 to activate the dynamic tables
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

auto calibrate_apic(u32 hz) -> u32 {
    volatile u32* lapic = reinterpret_cast<u32*>(0xFEE00000);

    // tell PIT to wait ~10ms
    // frequency is 1193182 hz. 10ms = 11931 ticks
    outb(0x43, 0x30); // channel 0, lo/hi, mode 0
    outb(0x40, 0x2B); // lo byte of 11931
    outb(0x40, 0x2E); // hi byte of 11931

    // start lapic timer
    lapic[0x380 / 4] = 0xFFFFFFFF; // max count

    // wait for pit to finish
    u8 status = 0;
    while (!(status & 0x80)) {
        outb(0x43, 0xE2); // read back command
        __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"(u16(0x40)));
    }

    u32 ticks_per_10ms = 0xFFFFFFFF - lapic[0x390 / 4]; // current count reg

    return ticks_per_10ms * 100 / hz;
}

static auto init_apic_timer() -> void {
    // mask legacy pic
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    volatile u32* lapic = reinterpret_cast<u32*>(0xFEE00000);
    lapic[0x0F0 / 4] = 0x1FF;             // software enable + spurious vector
    lapic[0x3E0 / 4] = 0x03;              // divide by 16
    lapic[0x320 / 4] = (1 << 17) | 32;    // periodic mode + vector 32
    lapic[0x380 / 4] = calibrate_apic(2); // initial count
}

static auto io_apic_write(u32 reg, u32 val) -> void {
    volatile u32* io_apic = reinterpret_cast<u32*>(0xFEC00000);
    io_apic[0] = reg; // select register
    io_apic[4] = val; // write value
}

static auto init_io_apic() -> void {
    // Mask legacy PICs
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    volatile u32* lapic = reinterpret_cast<u32*>(0xFEE00000);
    u32 const my_id = (lapic[0x020 / 4] >> 24) & 0xFF;

    // Vector 33 (0x21)
    // Bit 13 = 0 (Polarity: Active High)
    // Bit 15 = 1 (Trigger Mode: Level)
    // Bit 16 = 0 (Unmasked)
    u32 const flags = 33 | (0 << 13) | (1 << 15);

    // Map Pin 1 (Standard) and Pin 2 (Override)
    u32 pins[] = {1};

    for (u32 pin : pins) {
        io_apic_write(0x10 + pin * 2, flags);
        io_apic_write(0x10 + pin * 2 + 1, my_id << 24);
    }
}

static auto init_keyboard_hardware() -> void {
    // Flush the controller
    for (int i = 0; i < 100; ++i) {
        if ((inb(0x64) & 0x01))
            inb(0x60);
    }

    // Send "Enable Scanning" command to the keyboard
    while (inb(0x64) & 0x02)
        ; // Wait for input buffer empty
    outb(0x60, 0xF4);
}

extern "C" auto osca_on_keyboard(u8 scancode) -> void;

extern "C" auto kernel_on_keyboard() -> void {
    // drain all pending scancodes from the controller
    while (inb(0x64) & 0x01) {
        u8 scancode = inb(0x60);
        osca_on_keyboard(scancode);
    }

    // acknowledge the interrupt to the local APIC
    volatile u32* lapic = reinterpret_cast<u32*>(0xFEE00000);
    lapic[0x0B0 / 4] = 0;
}

extern "C" auto osca_on_timer() -> void;

extern "C" auto kernel_on_timer() -> void {
    osca_on_timer();

    // acknowledge interrupt
    *reinterpret_cast<volatile u32*>(0xFEE000B0) = 0;
}

// assembler functions
extern "C" auto kernel_apic_timer_handler() -> void;
extern "C" auto kernel_keyboard_handler() -> void;

extern "C" [[noreturn]] auto kernel_init(FrameBuffer fb, MemoryMap map)
    -> void {

    frame_buffer = fb;
    memory_map = map;
    heap = make_heap();

    GDTDescriptor gdt_desc{.size = sizeof(GDT) - 1,
                           .offset = reinterpret_cast<u64>(&gdt)};

    serial_print("kernel_load_gdt\r\n");
    kernel_load_gdt(&gdt_desc);

    serial_print("Stack location: ");
    serial_print_hex(reinterpret_cast<u64>(kernel_stack));
    serial_print("\r\n");

    serial_print("Heap start: ");
    serial_print_hex(reinterpret_cast<u64>(heap.start));
    serial_print("\r\n");

    serial_print("Memory Map Buffer: ");
    serial_print_hex(reinterpret_cast<u64>(memory_map.buffer));
    serial_print("\r\n");

    serial_print("init_paging\r\n");
    init_paging();

    // set idt entry 32
    u64 apic_addr = u64(kernel_apic_timer_handler);
    idt[32] = {u16(apic_addr),       0x08, 0, 0x8E, u16(apic_addr >> 16),
               u32(apic_addr >> 32), 0};
    u64 kbd_addr = u64(kernel_keyboard_handler);
    idt[33] = {u16(kbd_addr),       0x08, 0, 0x8E, u16(kbd_addr >> 16),
               u32(kbd_addr >> 32), 0};

    serial_print("idt_entry (timer, keyboard)\r\n");
    IDTR idtr = {sizeof(idt) - 1, reinterpret_cast<u64>(idt)};
    asm volatile("lidt %0" : : "m"(idtr));

    serial_print("init_apic_timer\r\n");
    init_apic_timer();

    serial_print("init_io_apic\r\n");
    init_io_apic();

    serial_print("init_keyboard_hardware\r\n");
    init_keyboard_hardware();

    asm volatile("sti");

    u64 stack_top = u64(kernel_stack) + sizeof(kernel_stack);
    serial_print("osca_start\r\n");
    osca_start(stack_top, osca);
}
