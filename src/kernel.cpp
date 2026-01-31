#include <efi.h>

#include "kernel.hpp"

// critical addresses:
// 0x0'8000 - ?       : start core trampoline code
// 0x1'0000 - 0x1'2fff: start core pml4 for protected mode code

FrameBuffer frame_buffer;
MemoryMap memory_map;
KeyboardConfig keyboard_config;
APIC apic;
Core cores[MAX_CORES];
u8 core_count = 0;
Heap heap;

// required by msvc/clang abi when floating-point arithmetic is used
extern "C" i32 _fltused;
extern "C" i32 _fltused = 0;

namespace {

// note: stack must be 16 byte aligned and top of stack sets RSP
//       make sure top of stack is 16 bytes aligned
alignas(16) static u8 kernel_stack[16384 * 16];

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
alignas(4096) u64 long_mode_pml4[512];

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
        auto pdp = get_next_table(long_mode_pml4, pml4_idx);
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
    // explicitly map the first 2MB as identity mapped (including 0x8000)
    map_range(0x0, 0x20'0000, RAM_FLAGS);

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
    asm volatile("mov %0, %%cr3" : : "r"(long_mode_pml4) : "memory");
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
                 : "r"(&kernel_stack[sizeof(kernel_stack)] - 8),
                   "r"(osca::start)
                 : "memory");
    // note: why -8?
    // the x86-64 system v abi requires the stack to be 16-byte aligned at the
    // point a call occurs. since a call pushes an 8-byte return address, the
    // compiler expects the stack to end in 0x8 upon entering a function.

    // the compiler is informed that this point is never reached
    __builtin_unreachable();
}

// verify low memory is actually usable
auto verify_low_memory() -> bool {
    // test critical addresses
    u64 test_addrs[] = {0x8000, 0x10000, 0x11000, 0x12000, 0x14000};

    for (auto addr : test_addrs) {
        volatile auto ptr = reinterpret_cast<volatile u32*>(addr);
        auto original = *ptr;

        *ptr = 0xDEADBEEF;
        sfence();

        if (*ptr != 0xDEADBEEF) {
            serial_print("memory test failed at: ");
            serial_print_hex(addr);
            serial_print("\n");
            return false;
        }

        *ptr = 0x12345678;
        mfence();

        if (*ptr != 0x12345678) {
            serial_print("memory test failed at: ");
            serial_print_hex(addr);
            serial_print("\n");
            return false;
        }

        *ptr = original; // Restore
    }

    serial_print("low memory verified\n");
    return true;
}

auto draw_rect(u32 x, u32 y, u32 width, u32 height, u32 color) -> void {
    for (auto i = y; i < y + height; ++i) {
        for (auto j = x; j < x + width; ++j) {
            frame_buffer.pixels[i * frame_buffer.stride + j] = color;
        }
    }
}

// In your global scope
extern "C" volatile u8 ap_boot_flag;
extern "C" volatile u8 ap_boot_flag = 0;

// this is the entry point for application processors
// each core lands here after the trampoline finishes
[[noreturn]] auto run_core() -> void {
    // flag bsp that core is running
    ap_boot_flag = 1;
    wbinvd();

    // ensure all stores are globally visible
    sfence();

    init_gdt();
    init_idt();

    // find this core index
    auto apic_id = (apic.local[0x20 / 4] >> 24) & 0xff;
    for (auto i = 0u; i < core_count; ++i) {
        if (cores[i].apic_id == apic_id) {
            osca::run_core(i);
        }
    }

    // core not found
    panic(0xffffff00);
}

auto delay_cycles(u64 cycles) -> void {
    for (auto i = 0u; i < cycles; ++i) {
        asm volatile("pause" ::: "memory");
    }
}

auto send_init_sipi(u8 apic_id, u32 trampoline_address) -> void {
    // select target core via high dword of icr
    apic.local[0x310 / 4] = u32(apic_id) << 24;

    // send init ipi to reset the ap (application processor)
    apic.local[0x300 / 4] = 0x00004500;

    // wait until the delivery status bit clears
    while (apic.local[0x300 / 4] & (1 << 12)) {
        asm volatile("pause");
    }

    // wait 10ms for ap to settle after reset (intel requirement)
    delay_cycles(10'000'000);

    // convert address to 4kb page vector; 0x8000 -> 0x08
    auto vector = (trampoline_address >> 12) & 0xFF;

    // re-select target apic id
    apic.local[0x310 / 4] = u32(apic_id) << 24;

    // send first sipi to wake ap at vector address
    apic.local[0x300 / 4] = 0x00004600 | vector;

    // send first sipi
    while (apic.local[0x300 / 4] & (1 << 12)) {
        asm volatile("pause");
    }

    // short 200us delay before retry (intel requirement)
    delay_cycles(200'000);

    // re-select target apic id (intel requirement)
    apic.local[0x310 / 4] = u32(apic_id) << 24;

    // send second sipi (intel requirement)
    apic.local[0x300 / 4] = 0x00004600 | vector;

    // final delivery check
    while (apic.local[0x300 / 4] & (1 << 12)) {
        asm volatile("pause");
    }
}

// addressed in the assembler code
extern "C" u8 kernel_asm_run_core_start[];
extern "C" u8 kernel_asm_run_core_end[];
extern "C" u8 kernel_asm_run_core_config[];

auto constexpr TRAMPOLINE_DEST = uptr(0x8000);

auto init_cores() {
    // the pages used in trampoline to transition from real -> protected -> long
    auto protected_mode_pml4 = reinterpret_cast<u64*>(0x1'0000);
    auto protected_mode_pdpt = reinterpret_cast<u64*>(0x1'1000);
    auto protected_mode_pd = reinterpret_cast<u64*>(0x1'2000);

    memset(protected_mode_pml4, 0, 4096);
    memset(protected_mode_pdpt, 0, 4096);
    memset(protected_mode_pd, 0, 4096);

    // identity map the first 1GB (for code/stack)
    protected_mode_pml4[0] = 0x1'1000 | PAGE_P | PAGE_RW;
    protected_mode_pdpt[0] = 0x1'2000 | PAGE_P | PAGE_RW;
    for (auto i = 0u; i < 32; ++i) {
        protected_mode_pd[i] = (i * 0x20'0000) | PAGE_P | PAGE_RW | PAGE_PS;
    }

    // flush caches so cores can see the pages
    wbinvd();

    for (auto i = 0u; i < core_count; ++i) {
        // skip the bsp (the core currently running this code)
        // usually the bsp has apic id 0, but check specifically
        auto bsp_id = (apic.local[0x020 / 4] >> 24) & 0xff;
        if (cores[i].apic_id == bsp_id) {
            continue;
        }

        // visual: bsp is starting to process core i (yellow)
        fill_rect(0, i * 15, 10, 10, 0xffffff00);

        // allocate a unique stack for this specific core
        auto stack = allocate_page();
        auto stack_top = reinterpret_cast<uptr>(stack) + 4096;

        // prepare the trampoline with the target function
        // calculate size using the addresses of the labels
        auto start_addr = uptr(kernel_asm_run_core_start);
        auto code_size = uptr(kernel_asm_run_core_end) - start_addr;

        // copy the trampoline code to lower 1MB so real mode can run it
        memcpy(reinterpret_cast<void*>(TRAMPOLINE_DEST),
               kernel_asm_run_core_start, code_size);

        // calculate the offset of the config data relative to the start
        auto config_offset = uptr(kernel_asm_run_core_config) - start_addr;

        // define struct
        struct [[gnu::packed]] TrampolineConfig {
            uptr protected_mode_pml4;
            uptr stack;
            uptr task;
            uptr long_mode_pml4;
        };
        auto config = reinterpret_cast<TrampolineConfig*>(TRAMPOLINE_DEST +
                                                          config_offset);

        // fill the values
        config->protected_mode_pml4 = uptr(protected_mode_pml4);
        config->stack = uptr(stack_top);
        config->task = uptr(run_core);
        config->long_mode_pml4 = uptr(long_mode_pml4);

        // visual: sipi sent (orange)
        fill_rect(10, i * 15, 10, 10, 0xffffa500);

        // the core sets flag to 1 once it has started
        ap_boot_flag = 0;
        wbinvd();

        // send the init-sipi-sipi sequence via the apic to start the core
        send_init_sipi(cores[i].apic_id, TRAMPOLINE_DEST);

        // visual: bsp entered wait loop (blue)
        fill_rect(20, i * 15, 10, 10, 0x0000ffff);

        serial_print("kicked core id: ");
        serial_print_hex_byte(cores[i].apic_id);
        serial_print("\n");

        // wait for core to start
        while (ap_boot_flag == 0) {
            asm volatile("pause");
        }
    }

    serial_print("all cores initialized\n");
}

} // namespace

[[noreturn]] auto kernel_start() -> void {
    init_serial();
    serial_print("serial initiated\n");

    if (!verify_low_memory()) {
        panic(0xffff0000);
    }
    screen_fill(0x00000000);

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

    serial_print("init_cores\n");
    init_cores();

    serial_print("osca_start\n");
    osca_start();
    // while (true) {
    //     asm volatile("hlt");
    // }
}
