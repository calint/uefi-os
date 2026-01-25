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

static GDT gdt{.null = {0, 0, 0, 0, 0, 0},
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
                result.start = reinterpret_cast<void*>(chunk_start);
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
    return reinterpret_cast<u64*>(entry & ~0xFFFULL);
}

static auto init_paging() -> void {
    u64* pml4 = reinterpret_cast<u64*>(allocate_page());

    // identity map 4GB using 2MB huge pages
    for (u64 addr = 0; addr < 0x100000000; addr += 0x200000) {
        u64 pml4_i = (addr >> 39) & 0x1FF;
        u64 pdp_i = (addr >> 30) & 0x1FF;
        u64 pd_i = (addr >> 21) & 0x1FF;

        u64* pdp = get_next_table(pml4[pml4_i]);
        u64* pd = get_next_table(pdp[pdp_i]);

        pd[pd_i] = addr | 0x83; // present + writable + huge
    }

    // specific 4KB map for LAPIC to disable caching
    u64 lapic_addr = 0xFEE00000;
    u64* pdp = get_next_table(pml4[(lapic_addr >> 39) & 0x1FF]);
    u64* pd = get_next_table(pdp[(lapic_addr >> 30) & 0x1FF]);

    u64* pt = reinterpret_cast<u64*>(allocate_page());
    pd[(lapic_addr >> 21) & 0x1FF] = reinterpret_cast<u64>(pt) | 0x03;
    pt[(lapic_addr >> 12) & 0x1FF] =
        lapic_addr | 0x1B; // present + writable + pwt + pcd

    // load new cr3
    asm volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
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
    // keyboard is usually irq 1
    io_apic_write(0x10 + 1 * 2, 33);
    // destination apic id (usually cpu 0)
    io_apic_write(0x10 + 1 * 2 + 1, 0);
}

extern "C" auto osca_on_keyboard(u8 scancode) -> void;

extern "C" auto kernel_on_keyboard() -> void {
    u8 scancode = inb(0x60);

    osca_on_keyboard(scancode);

    // send eoi to local apic
    *reinterpret_cast<volatile u32*>(0xFEE000B0) = 0;
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

    asm volatile("sti");

    u64 stack_top = u64(kernel_stack) + sizeof(kernel_stack);
    serial_print("osca_start\r\n");
    osca_start(stack_top, osca);
}
