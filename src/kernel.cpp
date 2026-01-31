#include <efi.h>

#include "kernel.hpp"

// note: stack must be 16 byte aligned and top of stack sets RSP
//       make sure top of stack is 16 bytes aligned
alignas(16) static u8 stack[16384 * 16];

FrameBuffer frame_buffer;
MemoryMap memory_map;
KeyboardConfig keyboard_config;
APIC apic;
Core cores[MAX_CORES];
u8 core_count = 0;
Heap heap;

// required by msvc/clang abi when floating-point arithmetic is used.
extern "C" i32 _fltused;
extern "C" i32 _fltused = 0;

namespace {

[[noreturn]] auto panic(u32 color) -> void {
    for (auto i = 0u; i < frame_buffer.stride * frame_buffer.height; ++i) {
        frame_buffer.pixels[i] = color;
    }
    // infinite loop so the hardware doesn't reboot
    asm volatile("cli");
    while (true) {
        asm("hlt");
    }
}

auto screen_fill(u32 color) -> void {
    for (auto i = 0u; i < frame_buffer.stride * frame_buffer.height; ++i) {
        frame_buffer.pixels[i] = color;
    }
}

auto fill_rect(u32 x, u32 y, u32 width, u32 height, u32 color) -> void {
    auto fb = frame_buffer.pixels;
    auto stride = frame_buffer.stride;

    // Bounds checking
    if (x >= frame_buffer.width || y >= frame_buffer.height)
        return;
    if (x + width > frame_buffer.width)
        width = frame_buffer.width - x;
    if (y + height > frame_buffer.height)
        height = frame_buffer.height - y;

    for (u32 i = 0; i < height; ++i) {
        for (u32 j = 0; j < width; ++j) {
            fb[(y + i) * stride + (x + j)] = color;
        }
    }
}

// serial (uart) init
auto inline init_serial() -> void {
    // ier (interrupt enable register): disable all hardware interrupts
    // avoids triple fault before idt is ready
    outb(0x3f8 + 1, 0x00);

    // lcr (line control register): set bit 7 (dlab) to 1
    // unlocks divisor registers at 0x3f8 and 0x3f9
    outb(0x3f8 + 3, 0x80);

    // dll/dlm (divisor latch low/high): set baud rate
    // 115200 / 3 = 38400 baud
    outb(0x3f8 + 0, 0x03);
    outb(0x3f8 + 1, 0x00);

    // lcr: 8 bits, no parity, 1 stop bit (8n1); dlab to 0
    // locks divisor and enables data transfer
    outb(0x3f8 + 3, 0x03);

    // fcr (fifo control register): enable/clear buffers
    // 14-byte threshold, clear transmit/receive queues
    outb(0x3f8 + 2, 0xc7);

    // mcr (modem control register): set rts/dtr
    // bit 3 enables auxiliary output 2 for irqs
    outb(0x3f8 + 4, 0x0b);
}

// sse (simd) init
auto init_sse() -> void {
    // cr0: control register 0
    u64 cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ull << 2); // clear em: disable x87 emulation
    cr0 |= (1ull << 1);  // set mp: monitor coprocessor (task switching)
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    // cr4: control register 4
    u64 cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9);  // set osfxsr: enable fxsave/fxrstor
    cr4 |= (1ull << 10); // set osxmmexcpt: enable simd exceptions (#xm)
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    // reset fpu state to defaults
    asm volatile("fninit");

    // mxcsr: control/status register for sse
    //  bits name   description
    //   0-5 flags  sticky bits set by hardware when errors occur
    //     6 daz    denormals are zero (treat inputs as 0)
    //  7-12 masks  set to 1 to ignore corresponding error
    // 13-14 rc     rounding control (00: nearest, 01: down, 10: up, 11: zero)
    //    15 ftz    flush to zero (treat tiny results as 0)
    u32 mxcsr = 0x1f80 | (1 << 15u);
    asm volatile("ldmxcsr %0" ::"m"(mxcsr));
}

// gdt (global descriptor table) init
auto init_gdt() -> void {
    // legacy x86 format; mostly ignored in 64-bit but fields must exist
    struct [[gnu::packed]] GDTEntry {
        u16 limit_low;
        u16 base_low;
        u8 base_middle;
        u8 access;      // p(1) dpl(2) s(1) type(4)
        u8 granularity; // g(1) d/b(1) l(1) avl(1) limit_high(4)
        u8 base_high;
    };

    struct [[gnu::packed]] GDT {
        GDTEntry null; // selector 0x00: required null descriptor
        GDTEntry code; // selector 0x08: kernel code (exec/read)
        GDTEntry data; // selector 0x10: kernel data (read/write)
    };

    // code access 0x9a: 10011010b (present, ring 0, code, exec/read)
    // code gran 0x20: 00100000b (l-bit set: marks 64-bit long mode)
    // data access 0x92: 10010010b (present, ring 0, data, read/write)
    alignas(8) auto static gdt = GDT{.null = {0, 0, 0, 0, 0, 0},
                                     .code = {0, 0, 0, 0x9a, 0x20, 0},
                                     .data = {0, 0, 0, 0x92, 0x00, 0}};

    // the 10-byte pointer passed to the lgdt instruction
    struct [[gnu::packed]] GDTDescriptor {
        u16 size;
        u64 offset;
    };

    auto descriptor =
        GDTDescriptor{.size = sizeof(GDT) - 1, .offset = u64(&gdt)};

    asm volatile("lgdt %0\n\t"         // load gdt register (gdtr)
                 "mov $0x10, %%ax\n\t" // 0x10 points to data descriptor
                 "mov %%ax, %%ds\n\t"  // load data segment
                 "mov %%ax, %%es\n\t"  // load extra segment
                 "mov %%ax, %%ss\n\t"  // load stack segment
                 "pushq $0x08\n\t"     // push code selector (0x08) for lretq
                 "lea 1f(%%rip), %%rax\n\t" // load address of label '1'
                 "pushq %%rax\n\t"          // push rip for lretq
                 "lretq\n\t" // far return: pops rip and cs to flush pipelin
                 "1:\n\t"    // now running with new cs/ds/ss
                 :
                 : "m"(descriptor)
                 : "rax", "memory");
}

// heap (bump allocator) init
// find biggest chunk of free memory and use it as heap
auto make_heap() -> Heap {
    // find largest contiguous chunk of memory
    auto largest_chunk_size = 0ull;
    auto aligned_start = 0ull;
    auto aligned_size = 0ull;

    // parse uefi memory descriptors
    auto desc = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map.buffer);
    auto num_descriptors = memory_map.size / memory_map.descriptor_size;

    for (auto i = 0u; i < num_descriptors; ++i) {
        // step by uefi-defined descriptor size
        auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(
            u64(desc) + (i * memory_map.descriptor_size));

        // usable ram not reserved by firmware/acpi
        if (d->Type == EfiConventionalMemory) {
            auto chunk_start = d->PhysicalStart;
            auto chunk_size = d->NumberOfPages * 4096;

            // find contiguous maximum for the heap
            if (chunk_size > largest_chunk_size) {
                largest_chunk_size = chunk_size;

                // 4k alignment: mask lower 12 bits to ensure page boundary
                aligned_start = (chunk_start + 4095) & ~4095ull;
                aligned_size = (chunk_size + 4095) & ~4095ull;
            }
        }
    }

    return {reinterpret_cast<void*>(aligned_start), aligned_size};
}

// pops a zeroed 4k buffer from the heap for structural paging
auto allocate_page() -> void* {
    // ensure heap has at least one 4k page remaining
    if (heap.size < 4096) {
        serial_print("error: out of memory for paging\n");
        panic(0xff00'0000); // red screen: fatal
    }

    auto ptr = heap.start;
    heap.start = reinterpret_cast<void*>(u64(heap.start) + 4096);
    heap.size -= 4096;
    memset(ptr, 0, 4096);
    return ptr;
}

// page table traversal
// returns pointer to the next level in paging hierarchy
// allocates a new zeroed page if the entry is not present
auto get_next_table(u64* table, u64 index) -> u64* {
    // check bit 0 (p): present
    if (!(table[index] & 0x01)) {
        // create next level only when needed
        void* next = allocate_page(); // zeroed 4KB chunk
        // link new table: set physical address and flags
        // 0x03: present | writable
        table[index] = reinterpret_cast<uptr>(next) | 0x03;
    }
    // mask lower 12 bits: remove flags to get pure physical address
    // x64 paging structures are always 4k aligned
    return reinterpret_cast<u64*>(table[index] & ~0xfffull);
}

// the top-level PML4 (512GB/entry) potentially covering 256 TB
alignas(4096) u64 boot_pml4[512];

// page table entry (pte) / page directory entry (pde) bits
// present (p): must be 1 to be a valid entry
auto constexpr PAGE_P = (1ull << 0);

// read/write (r/w): 0 = read-only, 1 = read/write
auto constexpr PAGE_RW = (1ull << 1);

// page-level write-through (pwt): bit 0 of pat index
auto constexpr PAGE_PWT = (1ull << 3);

// page-level cache disable (pcd): bit 1 of pat index
auto constexpr PAGE_PCD = (1ull << 4);

// page size (ps): 1 in pde (level 2) indicates 2mb huge page
auto constexpr PAGE_PS = (1ull << 7);

// pat (page attribute table) bit locations
// the pat bit is the "high bit" (bit 2) of the 3-bit pat index
// its position changes based on the page size!

// pat bit for 4kb ptes
auto constexpr PAGE_PAT_4KB = (1ull << 7);

// pat bit for 2mb pdes
auto constexpr PAGE_PAT_2MB = (1ull << 12);

// bit 12 in 'flags' parameter is a software-only signal that the caller wants
// write-combining (pat index 4)
auto constexpr USE_PAT_WC = (1ull << 12);

// range mapping with hybrid page sizes
// creates identity mappings with optimized page sizes
auto map_range(u64 phys, u64 size, u64 flags) -> void {
    // page alignment: floor start and ceil end to 4kb boundaries
    auto addr = phys & ~0xfffull;
    auto end = (phys + size + 4095) & ~0xfffull;

    while (addr < end) {
        // x64 virtual address bit-fields for table indexing
        // page map level 4
        auto pml4_idx = (addr >> 39) & 0x1ff;
        // page directory pointer
        auto pdp_idx = (addr >> 30) & 0x1ff;
        // page directory
        auto pd_idx = (addr >> 21) & 0x1ff;
        // page table
        auto pt_idx = (addr >> 12) & 0x1ff;

        // traverse hierarchy: allocate lower tables as needed
        auto pdp = get_next_table(boot_pml4, pml4_idx);
        auto pd = get_next_table(pdp, pdp_idx);

        // check if 2mb mapping is possible: aligned start and sufficient size
        if ((addr % 0x20'0000 == 0) && (end - addr >= 0x20'0000)) {
            // huge page: ps (page size) bit = 1 in page directory entry
            auto entry_flags = flags | PAGE_PS;

            if (flags & USE_PAT_WC) {
                // write-combining (index 4): binary 100
                // for 2mb pages, pat bit is bit 12
                entry_flags &= ~PAGE_PWT;
                entry_flags &= ~PAGE_PCD;
                entry_flags |= PAGE_PAT_2MB;
            }

            pd[pd_idx] = addr | entry_flags;
            addr += 0x20'0000; // jump by 2mb
        } else {
            // standard page: leaf exists at level 1 (pt)
            auto pt = get_next_table(pd, pd_idx);
            auto entry_flags = flags;

            if (flags & USE_PAT_WC) {
                // write-combining (index 4): binary 100
                // for 4kb pages, pat bit is bit 7
                entry_flags &= ~PAGE_PWT;
                entry_flags &= ~PAGE_PCD;
                entry_flags &= ~USE_PAT_WC;
                entry_flags |= PAGE_PAT_4KB;
            }

            pt[pt_idx] = addr | entry_flags;
            addr += 0x1000;
        }
    }
}

// identity maps uefi memory, sets pat, and activates cr3
auto init_paging() -> void {
    // preserve heap metadata before allocating page tables
    auto heap_start = u64(heap.start);
    auto heap_size = heap.size;

    // page attribute flags
    // p: present; rw: read/write
    auto constexpr RAM_FLAGS = PAGE_P | PAGE_RW;

    // pcd: page-level cache disable
    // essential for mmio to avoid reading stale hardware register values
    auto constexpr MMIO_FLAGS = PAGE_P | PAGE_RW | PAGE_PCD;

    // parse uefi memory map to identity-map system ram and firmware regions
    auto desc = static_cast<EFI_MEMORY_DESCRIPTOR*>(memory_map.buffer);
    auto num_descriptors = memory_map.size / memory_map.descriptor_size;
    for (auto i = 0u; i < num_descriptors; ++i) {
        auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(
            u64(desc) + (i * memory_map.descriptor_size));

        if ((d->Type == EfiACPIReclaimMemory) ||
            (d->Type == EfiACPIMemoryNVS)) {
            // acpi tables: must be mapped to parse hardware config later
            serial_print("* acpi tables\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, RAM_FLAGS);
        } else if ((d->Type == EfiLoaderCode) || (d->Type == EfiLoaderData) ||
                   (d->Type == EfiBootServicesCode) ||
                   (d->Type == EfiBootServicesData)) {
            // kernel binary + current uefi stack
            // note: EfiBootServiceCode and Data is mapped because current stack
            //       is there
            serial_print("* loaded kernel and current stack\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, RAM_FLAGS);
        } else if (d->Type == EfiConventionalMemory) {
            // general purpose ra
            serial_print("* memory\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, RAM_FLAGS);
        } else if (d->Type == EfiMemoryMappedIO) {
            // generic hardware mmio regions
            serial_print("* mmio region\n");
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, MMIO_FLAGS);
        }
    }

    serial_print("* apic mmio\n");
    // map apic registers for interrupt handling
    map_range(u64(apic.io), 0x1000, MMIO_FLAGS);
    map_range(u64(apic.local), 0x1000, MMIO_FLAGS);

    serial_print("* frame buffer\n");
    // map frame buffer with write-combining (pat index 4)
    auto constexpr FB_FLAGS = PAGE_P | PAGE_RW | USE_PAT_WC;
    map_range(u64(frame_buffer.pixels),
              frame_buffer.stride * frame_buffer.height * sizeof(u32),
              FB_FLAGS);

    serial_print("* heap\n");
    // map the dynamic memory pool
    map_range(heap_start, heap_size, RAM_FLAGS);

    serial_print("* trampoline\n");
    // Explicitly map the first 1MB as identity mapped (including 0x8000)
    map_range(0x0, 0x100000, RAM_FLAGS);

    // config pat: set pa4 to write-combining (0x01)
    // msr 0x277: ia32_pat register
    u32 low;
    u32 high;
    // rdmsr: read 64-bit model specific register into edx:eax
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277));

    // pat index 4 (pa4): bits 32-34 of the 64-bit msr
    // 0x01: write-combining (wc) mode
    // wc is essential for framebuffers; it buffers writes to the gpu
    high = (high & ~0x07u) | 0x01u;

    // wrmsr: write edx:eax back to ia32_pat
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(0x277));

    // wbinvd: write back and invalidate cache
    // ensures no stale cache lines exist after attribute change
    asm volatile("wbinvd" ::: "memory");

    // activate the new tables
    asm volatile("mov %0, %%cr3" : : "r"(boot_pml4) : "memory");
}

// apic timer calibration
auto calibrate_apic(u32 hz) -> u32 {
    // pit channel 0: set to mode 0 (interrupt on terminal count)
    // frequency: 1193182 hz; 10ms = ~11931 ticks (0x2e2b)
    outb(0x43, 0x30); // control: ch0, lo/hi, mode 0, binary
    outb(0x40, 0x2b); // divisor low byte
    outb(0x40, 0x2e); // divisor high byte

    // lapic initial count register (0x380): set to max
    // timer begins counting down immediately
    apic.local[0x380 / 4] = 0xffff'ffff; // max count

    // polling pit status via read-back command (0xe2)
    // bit 7 is set when the pit terminal count is reached (10ms elapsed)
    auto status = 0;
    while (!(status & 0x80)) {
        outb(0x43, 0xe2); // read-back status for ch0
        status = inb(0x40);
    }
    // lapic current count register (0x390): read remaining ticks
    auto current_count = apic.local[0x390 / 4];
    auto ticks_per_10ms = 0xffff'ffff - current_count;

    // calculate divisor: (ticks in 10ms * 100) / target_hz
    // result is the initial count value for periodic interrupts
    return ticks_per_10ms * 100 / hz;
}

// disables legacy pic and starts lapic timer in periodic mode
auto init_timer() -> void {
    // disable legacy pic: mask all interrupts on master (0x21) and slave (0xa1)
    // essential to prevent "spurious" interrupts from deprecated hardware
    outb(0x21, 0xff);
    outb(0xa1, 0xff);

    // svr (spurious interrupt vector register): software enable lapic
    // 0x1ff: set bit 8 (apic software enable) and bits 0-7 (vector 255)
    apic.local[0x0f0 / 4] = 0x1ff;

    // dcr (divide configuration register): set timer divisor
    // 0x03: divide by 16 (timer increments every 16 bus cycles)
    apic.local[0x3e0 / 4] = 0x03;

    // lvt timer register: configure mode and vector
    // bit 17 (1 << 17): periodic mode (auto-reloads count)
    // bits 0-7 (32): vector index in idt for timer interrupts
    apic.local[0x320 / 4] = (1 << 17) | 32;

    // icr (initial count register): set the countdown start value
    // uses calibration logic to determine 2hz (0.5s interval)
    apic.local[0x380 / 4] = calibrate_apic(2);
}

// io-apic register access
// writes to an io-apic register using the index/data window
auto io_apic_write(u32 reg, u32 val) -> void {
    // ioregsel (offset 0x00): select the target register index
    apic.io[0] = reg;

    // iowin (offset 0x10): write the 32-bit data to the selected register
    // note: offset 0x10 is index [4] in a u32 array (4 * 4 bytes)
    apic.io[4] = val; // write value
}

// keyboard and io-apic routing
// routes keyboard irq through io-apic and enables scanning
auto init_keyboard() -> void {
    // get local apic id of the current cpu (bits 24-31 of offset 0x020)
    auto cpu_id = (apic.local[0x020 / 4] >> 24) & 0xff;

    // configure io-apic redirection table for keyboard (usually gsi 1)
    // index 0x10 is the start of the redirection table (2 x 32-bit registers
    // per entry) low 32 bits: vector 33 | flags (trigger mode, polarity, etc.)
    io_apic_write(0x10 + keyboard_config.gsi * 2, 33 | keyboard_config.flags);
    // high 32 bits: destination field (sets which cpu receives the interrupt)
    io_apic_write(0x10 + keyboard_config.gsi * 2 + 1, cpu_id << 24);

    // flush: clear the output buffer (port 0x60) of any stale data
    // check status register (port 0x64) bit 0 (output buffer full)
    auto flush_count = 0u;
    while (inb(0x64) & 0x01) {
        inb(0x60);
        if (++flush_count > 100) {
            serial_print("kbd: flush timeout\n");
            break;
        }
    }

    // wait for controller: check bit 1 (input buffer full)
    // cannot send commands until this bit is 0
    auto wait_count = 0u;
    while (inb(0x64) & 0x02) {
        if (++wait_count > 100000) {
            serial_print("kbd: controller timeout\n");
            return;
        }
        asm volatile("pause");
    }

    // send command 0xf4: enable scanning
    // tells the keyboard to start sending scancodes when keys are pressed
    outb(0x60, 0xf4);

    // diagnostic: wait for 0xfa (acknowledge) from the keyboard
    auto ack_received = false;
    for (auto i = 0u; i < 1'000'000; ++i) {
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
        serial_print("warning: kbd did not ack\n");
    }
}

// callback assembler functions
extern "C" auto kernel_asm_timer_handler() -> void;
extern "C" auto kernel_asm_keyboard_handler() -> void;

// idt (interrupt descriptor table) init
auto init_idt() -> void {
    // 16-byte descriptor format for x86-64
    struct [[gnu::packed]] IDTEntry {
        u16 low;
        u16 sel;
        u8 ist;
        u8 attr;
        u16 mid;
        u32 high;
        u32 res;
    };

    // alignas(16): required for performance and hardware consistency
    alignas(16) static IDTEntry idt[256];

    // set idt entry 32 (lapic timer)
    // 0x8e: 10001110b -> p=1, dpl=00, type=1110 (64-bit interrupt gate)
    // p : present
    // dpl: ring 0
    // type: disable nested interrupts
    // 8: second entry in the gdt (code)
    auto apic_addr = u64(kernel_asm_timer_handler);
    idt[32] = {u16(apic_addr),       8, 0, 0x8e, u16(apic_addr >> 16),
               u32(apic_addr >> 32), 0};

    // set idt entry 33 (keyboard)
    auto kbd_addr = u64(kernel_asm_keyboard_handler);
    idt[33] = {u16(kbd_addr),       8, 0, 0x8e, u16(kbd_addr >> 16),
               u32(kbd_addr >> 32), 0};

    // idtr: the 10-byte structure passed to 'lidt'
    struct [[gnu::packed]] IDTR {
        u16 limit;
        u64 base;
    };

    auto idtr = IDTR{sizeof(idt) - 1, u64(idt)};

    // lidt: load the interrupt descriptor table register
    asm volatile("lidt %0" : : "m"(idtr));
}

// keyboard interrupt handler
// c-linkage handler called by assembly isr stub
extern "C" auto kernel_on_keyboard() -> void {
    // drain ps/2 output buffer: bit 0 of status (0x64) means data is waiting
    // reading all pending bytes prevents the controller from getting "stuck"
    while (inb(0x64) & 0x01) {
        // read raw byte from data port
        auto scancode = inb(0x60);

        // diagnostic: log scancode to serial for debugging
        serial_print("|");
        serial_print_hex_byte(scancode);
        serial_print("|");

        // pass scancode to the operating system's input layer
        osca::on_keyboard(scancode);
    }

    // eoi (end of interrupt): writing 0 to offset 0x0b0
    // notifies the lapic that the handler is finished so it can deliver
    // the next interrupt of equal or lower priority
    apic.local[0x0B0 / 4] = 0;
}

// lapic timer interrupt handler
// c-linkage handler called by the assembly timer stub
extern "C" auto kernel_on_timer() -> void {
    // notify the os layer that a tick has occurred
    // typically used for task switching, sleep timers, or profiling
    osca::on_timer();

    // eoi (end of interrupt): writing 0 to offset 0x0b0
    // essential to clear the 'in-service' bit in the lapic
    // failure to do this will prevent any further timer interrupts
    apic.local[0x0B0 / 4] = 0;
}

// jumping to the os entry point
[[noreturn]] auto osca_start() -> void {
    // pivot: load the new stack pointer (rsp) and base pointer (rbp)
    // jump: perform an absolute indirect jump to the os entry function
    asm volatile("mov %0, %%rsp\n\t"
                 "mov %0, %%rbp\n\t"
                 "jmp *%1"
                 :
                 : "r"(&stack[sizeof(stack)] - 8), "r"(osca::start)
                 : "memory");
    // note: why -8?
    // the x86-64 system v abi requires the stack to be 16-byte aligned at the
    // point a call occurs. since a call pushes an 8-byte return address, the
    // compiler expects the stack to end in 0x8 upon entering a function.

    // the compiler is informed that this point is never reached
    __builtin_unreachable();
}

} // namespace

// addressed in the assembler code
extern "C" u8 trampoline_start[];
extern "C" u8 trampoline_end[];
extern "C" u8 trampoline_config_data[];

auto kernel_start_task(u64 pml4_phys, u64 stack_phys, auto (*target)()->void)
    -> void {
    const uptr base = 0x8000;

    // 1. Calculate size using the addresses of the labels
    uptr start_addr = reinterpret_cast<uptr>(trampoline_start);
    uptr end_addr = reinterpret_cast<uptr>(trampoline_end);
    uptr code_size = end_addr - start_addr;

    // 2. Copy the code.
    // Since trampoline_start is an array, it is already the source address.
    memcpy(reinterpret_cast<void*>(base), trampoline_start, code_size);

    // 3. Calculate the offset of the config data relative to the start
    uptr config_label_addr = reinterpret_cast<uptr>(trampoline_config_data);
    uptr config_offset = config_label_addr - start_addr;

    // 4. Get the pointer to the config struct WITHIN the 0x8000 memory area
    struct [[gnu::packed]] TrampolineConfig {
        u64 pml4;
        u64 stack_address; // initial rsp for the ap
        u64 entry_point;   // address of kernel_ap_main
        u64 final_pml4;
        u64 fb_physical;
    };
    auto* config = reinterpret_cast<TrampolineConfig*>(base + config_offset);

    // 5. Fill the values
    config->pml4 = pml4_phys;
    config->stack_address = stack_phys;
    config->entry_point = u64(target);
    config->final_pml4 = u64(boot_pml4);
    config->fb_physical = u64(frame_buffer.pixels);

    // Ensure the data is actually in RAM before we kick the AP
    asm volatile("mfence" ::: "memory");
    asm volatile("wbinvd" ::: "memory"); // Flush all caches
}

// In your global scope
extern "C" volatile u8 ap_boot_flag;
extern "C" volatile u8 ap_boot_flag = 0;

auto draw_rect(u32 x, u32 y, u32 width, u32 height, u32 color) -> void {
    for (u32 i = y; i < y + height; ++i) {
        for (u32 j = x; j < x + width; ++j) {
            frame_buffer.pixels[i * frame_buffer.stride + j] = color;
        }
    }
}

// This is the entry point for Application Processors
// Each core lands here after the trampoline finishes
[[noreturn]] auto ap_main() -> void {
    ap_boot_flag = 1;
    // asm volatile("mfence" ::: "memory"); // Ensure write is visible to BSP
    // asm volatile("wbinvd" ::: "memory"); // Extra aggressive flush

    serial_print("AP\n");

    init_gdt(); // Each core needs its own GDT state
    init_idt(); // Each core needs its own IDTR loaded

    serial_print("AP Core Online\n");
    // 2. Identify this core index to determine screen position
    // We read the local APIC ID to know who we are
    u32 my_apic_id = (apic.local[0x20 / 4] >> 24) & 0xFF;
    u32 core_index = 0;
    for (u32 i = 0; i < core_count; ++i) {
        if (cores[i].apic_id == my_apic_id) {
            core_index = i;
            break;
        }
    }

    // 3. Draw a unique rectangle for this core
    // Each core gets a 50x50 block separated by 10 pixels
    u32 x_pos = core_index * 60;
    u32 y_pos = 300;

    // Color logic: Generate a color based on ID (e.g., Greenish-Blue)
    u32 color = 0xFF00FF00 | (my_apic_id * 0x1234);

    // interrupts_enable();
    while (true) {
        draw_rect(x_pos, y_pos, 50, 50, color);
        ++color;
        // CRITICAL: Ensure the writes are visible to the GPU
        // mfence forces memory ordering, and wbinvd flushes all caches
        asm volatile("mfence" ::: "memory");
        asm volatile("wbinvd" ::: "memory");
        //        asm("hlt");
    }
}

auto delay_cycles(u64 cycles) -> void {
    for (u64 i = 0; i < cycles; i++) {
        asm volatile("pause" ::: "memory");
    }
}

// FIX 1: Send TWO SIPIs (Intel requirement)
auto send_init_sipi(u8 apic_id, u32 trampoline_address) -> void {
    // 1. Send INIT IPI
    apic.local[0x310 / 4] = (static_cast<u32>(apic_id) << 24);
    apic.local[0x300 / 4] = 0x00004500;

    while (apic.local[0x300 / 4] & (1 << 12)) {
        asm volatile("pause");
    }

    // 10ms delay after INIT (Intel requirement)
    delay_cycles(10'000'000);

    // 2. Send FIRST SIPI
    u32 vector = (trampoline_address >> 12) & 0xFF;
    apic.local[0x310 / 4] = (static_cast<u32>(apic_id) << 24);
    apic.local[0x300 / 4] = 0x00004600 | vector;

    while (apic.local[0x300 / 4] & (1 << 12)) {
        asm volatile("pause");
    }

    // 200us delay between SIPIs (Intel spec)
    delay_cycles(200'000);

    // 3. Send SECOND SIPI (CRITICAL - Intel requires TWO)
    apic.local[0x310 / 4] = (static_cast<u32>(apic_id) << 24);
    apic.local[0x300 / 4] = 0x00004600 | vector;

    while (apic.local[0x300 / 4] & (1 << 12)) {
        asm volatile("pause");
    }
}

auto kernel_start_cores() {
    serial_print("start cores\n");

    u64* bridge_pml4 = reinterpret_cast<u64*>(0x10000);
    u64* bridge_pdpt = reinterpret_cast<u64*>(0x11000);
    u64* bridge_pd = reinterpret_cast<u64*>(0x12000);
    // Let's use 0x13000 for a second Page Directory to map the FB
    u64* bridge_fb_pd = reinterpret_cast<u64*>(0x13000);

    memset(bridge_pml4, 0, 4096);
    memset(bridge_pdpt, 0, 4096);
    memset(bridge_pd, 0, 4096);
    memset(bridge_fb_pd, 0, 4096);

    // 1. Identity map the first 1GB (for code/stack)
    bridge_pml4[0] = 0x11000 | 0x3;
    bridge_pdpt[0] = 0x12000 | 0x3;
    for (u64 i = 0; i < 32; ++i) {
        bridge_pd[i] = (i * 0x200000) | 0x83;
    }

    // 2. Identity map the Framebuffer in the Bridge tables
    u64 fb_phys = reinterpret_cast<uptr>(frame_buffer.pixels);
    u64 fb_size = frame_buffer.stride * frame_buffer.height * sizeof(u32);

    // Find which 1GB slot the FB belongs to
    u64 fb_pdpt_idx = (fb_phys >> 30) & 0x1FF;
    bridge_pdpt[fb_pdpt_idx] = reinterpret_cast<uptr>(bridge_fb_pd) | 0x3;

    // Map the FB range using 2MB pages
    u64 fb_start_2mb = fb_phys >> 21;
    u64 fb_pages = (fb_size + 0x1FFFFF) >> 21;
    for (u64 i = 0; i < fb_pages; ++i) {
        u64 current_phys = (fb_start_2mb + i) << 21;
        bridge_fb_pd[(current_phys >> 21) & 0x1FF] = current_phys | 0x83;
    }

    asm volatile("wbinvd" ::: "memory");

    for (u8 i = 0; i < core_count; ++i) {
        // Skip the BSP (the core currently running this code)
        // Usually the BSP has APIC ID 0, but we check specifically
        auto bsp_id = (apic.local[0x020 / 4] >> 24) & 0xff;
        if (cores[i].apic_id == bsp_id) {
            continue;
        }

        // Visual: BSP is starting to process Core i (Yellow)
        fill_rect(0, i * 15, 10, 10, 0xFFFFFF00);
        ap_boot_flag = 0;
        asm volatile("mfence" ::: "memory");

        // 1. Allocate a unique stack for this specific core
        // Each core gets its own 4KB page
        void* ap_stack = allocate_page();
        u64 stack_top = reinterpret_cast<uptr>(ap_stack) + 4096;

        // 2. Prepare the trampoline with the target function
        kernel_start_task(u64(bridge_pml4), stack_top, ap_main);

        // Visual: SIPI Sent (Orange)
        fill_rect(10, i * 15, 10, 10, 0xFFFFa500);

        // 3. Send the INIT-SIPI-SIPI sequence via the APIC
        // We use the APIC ID found in the MADT
        send_init_sipi(cores[i].apic_id, 0x8000);

        // Visual: BSP entered wait loop (Blue)
        fill_rect(20, i * 15, 10, 10, 0x0000ffff);

        serial_print("Kicked core ID: ");
        serial_print_hex_byte(cores[i].apic_id);
        serial_print("\n");

        // mfence BEFORE reading flag
        u64 timeout = 0;
        while (true) {
            // Ensure we see the latest value
            asm volatile("mfence" ::: "memory");

            volatile u8 flag = ap_boot_flag;
            if (flag != 0) {
                break;
            }

            asm volatile("pause");

            if (++timeout > 0x10000000) {
                // Visual: Red (timeout)
                fill_rect(30, i * 15, 10, 10, 0xffff0000);
                serial_print("TIMEOUT\n");
                break;
            }

            // Heartbeat every 16M iterations
            if ((timeout & 0xFFFFFF) == 0) {
                fill_rect(30, i * 15, 10, 10, 0xff444444);
            }
        }

        if (ap_boot_flag == 1) {
            // Visual: AP SUCCESS (Green)
            fill_rect(40, i * 15, 10, 10, 0xff00ff00);
        }
    }

    serial_print("All cores initialized.\n");

    // screen_fill(0xFF00FF00);
    if (ap_boot_flag != 0) {
        asm volatile("cli");
        asm volatile("hlt");
    }
}

// FIX 4: Verify low memory is actually usable
// Add this check in kernel_start() before kernel_start_cores():
auto verify_low_memory() -> bool {
    // Test critical addresses
    u64 test_addrs[] = {0x8000, 0x10000, 0x11000, 0x12000, 0x14000};

    for (auto addr : test_addrs) {
        volatile u32* ptr = reinterpret_cast<volatile u32*>(addr);
        u32 original = *ptr;

        *ptr = 0xDEADBEEF;
        asm volatile("mfence" ::: "memory");

        if (*ptr != 0xDEADBEEF) {
            serial_print("Memory test failed at: ");
            serial_print_hex(addr);
            serial_print("\n");
            return false;
        }

        *ptr = 0x12345678;
        asm volatile("mfence" ::: "memory");

        if (*ptr != 0x12345678) {
            serial_print("Memory test failed at: ");
            serial_print_hex(addr);
            serial_print("\n");
            return false;
        }

        *ptr = original; // Restore
    }

    serial_print("Low memory verified\n");
    return true;
}

[[noreturn]] auto kernel_start() -> void {
    if (!verify_low_memory()) {
        panic(0xffff0000);
    }
    screen_fill(0x00000000);

    // TEST: Can we actually use 0x8000?
    volatile u32* test_ptr = reinterpret_cast<volatile u32*>(0x8000);
    *test_ptr = 0xDEADBEEF;
    if (*test_ptr != 0xDEADBEEF) {
        panic(0xFFFF0000);
    }
    // If we get here, 0x8000 is likely safe for now.

    init_serial();
    serial_print("serial initiated\n");

    heap = make_heap();

    serial_print("enable_sse\n");
    init_sse();

    serial_print("init_gdt\n");
    init_gdt();

    serial_print("init_paging\n");
    init_paging();

    serial_print("init_idt\n");
    init_idt();

    serial_print("init_timer\n");
    init_timer();

    serial_print("init_keyboard\n");
    init_keyboard();

    serial_print("kernel_start_cores\n");
    kernel_start_cores();

    serial_print("osca_start\n");
    //    osca_start();
    while (true) {
        asm volatile("hlt");
    }
}
