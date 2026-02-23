#include <efi.h>

#include "atomic.hpp"
#include "config.hpp"
#include "kernel.hpp"

// * unexpected conditions reboot the system
// * no recovery paths implemented
// * correctness assumed

using namespace kernel;

namespace {

// note: stack must be 16 byte aligned and top of stack sets RSP
//       make sure top of stack is 16 bytes aligned
alignas(16) static u8 kernel_stack[4096];

// serial (uart) init
auto inline init_serial() -> void {
    // lcr (line control register): set bit 7 (dlab) to 1
    // unlocks divisor registers at 0x3f8 and 0x3f9
    outb(0x3f8 + 3, 0x80);

    // dll/dlm (divisor latch low/high): set baud rate 115200 baud
    // note: divisor is 115200 / target baud = 1
    outb(0x3f8 + 0, 1);
    outb(0x3f8 + 1, 0);

    // lcr: 8 bits, no parity, 1 stop bit (8n1); dlab to 0
    // locks divisor and enables data transfer
    outb(0x3f8 + 3, 0x03);
}

// fpu/simd (sse & avx) init
// assumes cpu supports sse + avx + xsave
auto inline init_fpu() -> void {
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
    cr4 |= (1ull << 18); // set osxsave: enable xsave and xgetbv/xsetbv
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    // xcr0: extended control register 0
    //  bits name   description
    //     0 x87    standard fpu state
    //     1 sse    xmm registers
    //     2 avx    ymm registers (upper 128 bits)
    u32 const eax = (1 << 0) | (1 << 1) | (1 << 2);
    u32 const edx = 0;
    asm volatile("xsetbv" : : "a"(eax), "d"(edx), "c"(0));

    // mxcsr: control/status register for sse
    //  bits name    description
    //   0-5 flags   sticky bits set by hardware when errors occur
    //     6 daz     denormals are zero (treat tiny input values as 0)
    //  7-12 masks   1 to ignore corresponding error (im, dm, zm, om, um, pm)
    // 13-14 rc      rounding control (00: nearest, 01: down, 10: up, 11: zero)
    //    15 ftz     flush to zero (treat tiny result values as 0)
    //
    // note: enabling daz (bit 6) and ftz (bit 15) prevents massive performance
    //       penalties from microcode assists when handling subnormal numbers
    // masks (bits 7-12) are set to 1 (0x1f80) to prevent exceptions
    auto const mxcsr = 0x1f80u | (1u << 6u) | (1u << 15u);
    asm volatile("ldmxcsr %0" ::"m"(mxcsr));
}

// gdt (global descriptor table) init
auto inline init_gdt() -> void {
    // code access 0x9a: 10011010b (present, ring 0, code, exec/read)
    // code gran 0x20: 00100000b (l-bit set: marks 64-bit long mode)
    // data access 0x92: 10010010b (present, ring 0, data, read/write)
    alignas(8) u64 const static gdt[] = {
        0,                     // null
        0x00209a0000000000ull, // 64-bit code
        0x0000920000000000ull  // data
    };

    struct [[gnu::packed]] {
        u16 size;
        u64 addr;
    } const gdtr{sizeof(gdt) - 1, u64(gdt)};

    asm volatile("lgdt %0\n"              // load gdt register (gdtr)
                 "mov $0x10, %%ax\n"      // 0x10 points to data descriptor
                 "mov %%ax, %%ds\n"       // load data segment
                 "mov %%ax, %%es\n"       // load extra segment
                 "mov %%ax, %%ss\n"       // load stack segment
                 "pushq $0x08\n"          // push code selector (0x08) for lretq
                 "lea 1f(%%rip), %%rax\n" // load address of label '1'
                 "pushq %%rax\n"          // push rip for lretq
                 "lretq\n" // far return: pops rip and cs to flush pipeline
                 "1:\n"    // now running with new cs/ds/ss
                 :
                 : "m"(gdtr)
                 : "rax", "memory");
}

auto constexpr PAGE_4K = 0x1000ull;
auto constexpr PAGE_2M = 0x20'0000ull;

// heap (bump allocator) init
// finds the largest contiguous usable memory chunk and aligns to page
// boundaries
auto make_heap() -> Heap {
    auto aligned_start = 0ull;
    auto aligned_size = 0ull;
    auto max_size = 0ull;

    auto const* const desc = ptr<EFI_MEMORY_DESCRIPTOR>(memory_map.buffer);
    auto const num_descriptors = memory_map.size / memory_map.descriptor_size;

    for (auto i = 0u; i < num_descriptors; ++i) {
        auto const* const d = ptr_offset<EFI_MEMORY_DESCRIPTOR>(
            uptr(desc), i * memory_map.descriptor_size);

        if (d->Type == EfiConventionalMemory) {
            auto const chunk_start = d->PhysicalStart;
            auto const chunk_end = chunk_start + (d->NumberOfPages * 4096);

            // align start up; align end down; ensures heap is within physical
            // bounds
            auto const current_start =
                (chunk_start + PAGE_4K - 1) & ~(PAGE_4K - 1);
            auto const current_end = chunk_end & ~(PAGE_4K - 1);

            if (current_end > current_start) {
                auto const current_size = current_end - current_start;
                if (current_size > max_size) {
                    max_size = current_size;
                    aligned_start = current_start;
                    aligned_size = current_size;
                }
            }
        }
    }

    // note: assumes heap was found
    return {ptr<void>(aligned_start), aligned_size};
}

// the top-level PML4 (512GB/entry) potentially covering 256 TB
alignas(4096) u64 long_mode_pml4[512];

// page table entry (pte) / page directory entry (pde) bits
// present (p): must be 1 to be a valid entry
auto constexpr static PAGE_P = (1ull << 0);

// read/write (r/w): 0 = read-only, 1 = read/write
auto constexpr static PAGE_RW = (1ull << 1);

// page-level write-through (pwt): bit 0 of pat index
auto constexpr static PAGE_PWT = (1ull << 3);

// page-level cache disable (pcd): bit 1 of pat index
auto constexpr static PAGE_PCD = (1ull << 4);

// page size (ps): 1 in pde (level 2) indicates 2mb huge page
auto constexpr static PAGE_PS = (1ull << 7);

// pat (page attribute table) bit locations
// the pat bit is the "high bit" (bit 2) of the 3-bit pat index
// its position changes based on the page size!

// pat bit for 4KB ptes
auto constexpr static PAGE_PAT_4KB = (1ull << 7);

// pat bit for 2MB pdes
auto constexpr static PAGE_PAT_2MB = (1ull << 12);

// bit 12:
// * for 2MB pages: hardware PAT bit
// * for 4KB pages: software signal translated to PAGE_PAT_4KB
auto constexpr static USE_PAT_WC = (1ull << 12);

// page table traversal
// returns pointer to the next level in paging hierarchy
// allocates a new zeroed page if the entry is not present
auto get_next_table(u64* const table, u64 const index) -> u64* {
    auto const entry = table[index];

    // check bit 0 (p): present
    if (!(entry & PAGE_P)) {
        // create next level only when needed
        auto const* const next = allocate_pages(1); // zeroed 4KB chunk
        // link new table: set physical address and flags
        table[index] = uptr(next) | PAGE_P | PAGE_RW;
    }
    // mask out lower 12 flag bits to obtain physical address
    return ptr<u64>(table[index] & ~(PAGE_4K - 1));
}

// range mapping with hybrid page sizes
// creates identity mappings with optimized page sizes
auto inline map_range(uptr const phys, u64 const size, u64 const flags)
    -> void {

    // page alignment: floor start and ceil end to 4KB boundaries
    auto addr = phys & ~(PAGE_4K - 1);
    auto const end = (phys + size + PAGE_4K - 1) & ~(PAGE_4K - 1);

    while (addr < end) {
        // x64 virtual address bit-fields for table indexing
        // page map level 4
        auto const pml4_idx = (addr >> 39) & 0x1ff;
        // page directory pointer
        auto const pdp_idx = (addr >> 30) & 0x1ff;
        // page directory
        auto const pd_idx = (addr >> 21) & 0x1ff;
        // page table
        auto const pt_idx = (addr >> 12) & 0x1ff;

        // traverse hierarchy: allocate lower tables as needed
        auto* const pdp = get_next_table(long_mode_pml4, pml4_idx);
        auto* const pd = get_next_table(pdp, pdp_idx);

        // check if 2MB page is possible
        // safe to overwrite if entry is not present or is already a huge page
        auto const entry = pd[pd_idx];
        auto const is_huge = (entry & PAGE_P) && (entry & PAGE_PS);
        auto const is_free = !(entry & PAGE_P);

        auto const can_use_2mb = (addr & (PAGE_2M - 1)) == 0 &&
                                 (addr + PAGE_2M <= end) &&
                                 (is_free || is_huge);

        if (can_use_2mb) {
            pd[pd_idx] = addr | flags | PAGE_PS;
            addr += PAGE_2M;
        } else {
            if (entry & PAGE_PS) {
                // current entry is marked as huge page
                // if flags are same as entry continue because this memory has
                // been mapped
                auto constexpr FLAG_MASK = PAGE_P | PAGE_RW | PAGE_PS |
                                           PAGE_PWT | PAGE_PCD | USE_PAT_WC;
                auto const existing_flags = entry & FLAG_MASK;
                auto const expected_flags = (flags | PAGE_PS) & FLAG_MASK;
                if (existing_flags != expected_flags) {
                    serial::print("error: 2MB page flag mismatch\n");
                    panic(0x00'ff'ff'00); // yellow
                }
                // jump to next 2MB page
                addr = (addr + PAGE_2M) & ~(PAGE_2M - 1);
                continue;
            }
            // current entry not a huge page
            auto* const pt = get_next_table(pd, pd_idx);
            auto const entry_flags = (flags & USE_PAT_WC)
                                         ? (flags & ~USE_PAT_WC) | PAGE_PAT_4KB
                                         : flags;
            pt[pt_idx] = addr | entry_flags;
            addr += PAGE_4K;
        }
    }
}

// maps uefi memory, sets pat, and activates cr3
auto init_paging() -> void {
    // preserve heap metadata before allocating page tables
    auto const heap_start = uptr(heap.start);
    auto const heap_size = heap.size;

    // flag that is true after scanning memory if trampoline and protected mode
    // pages are in free memory
    auto trampoline_memory_is_free = false;

    // page attribute flags
    // p: present; rw: read/write
    auto constexpr static RAM_FLAGS = PAGE_P | PAGE_RW;

    // pcd: page-level cache disable
    // essential for mmio to avoid reading stale hardware register values
    auto constexpr static MMIO_FLAGS = PAGE_P | PAGE_RW | PAGE_PCD;

    // parse uefi memory map to identity-map system ram and firmware regions
    auto total_mem_B = 0ull;
    auto free_mem_B = 0ull;
    auto const* const desc = ptr<EFI_MEMORY_DESCRIPTOR>(memory_map.buffer);
    auto const num_descriptors = memory_map.size / memory_map.descriptor_size;
    for (auto i = 0u; i < num_descriptors; ++i) {
        auto const* const d = ptr_offset<EFI_MEMORY_DESCRIPTOR>(
            desc, i * memory_map.descriptor_size);

        if (d->Type == EfiACPIReclaimMemory || d->Type == EfiACPIMemoryNVS) {
            // acpi tables: must be mapped to parse hardware config later
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, RAM_FLAGS);
            total_mem_B += d->NumberOfPages * 4096;
        } else if (d->Type == EfiLoaderCode || d->Type == EfiLoaderData ||
                   d->Type == EfiBootServicesCode ||
                   d->Type == EfiBootServicesData) {
            // kernel binary + current uefi stack
            // note: EfiBootServiceCode and Data is mapped because current stack
            //       is there for now
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, RAM_FLAGS);
            total_mem_B += d->NumberOfPages * 4096;
        } else if (d->Type == EfiConventionalMemory) {
            // general purpose ram
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, RAM_FLAGS);
            total_mem_B += d->NumberOfPages * 4096;
            free_mem_B += d->NumberOfPages * 4096;
            // check if range covers the trampoline and page table ram
            if (d->PhysicalStart <= 0x8000 &&
                d->PhysicalStart + d->NumberOfPages * 4096 >= 0x1'2000) {
                trampoline_memory_is_free = true;
            }
        } else if (d->Type == EfiMemoryMappedIO) {
            // generic hardware mmio regions
            map_range(d->PhysicalStart, d->NumberOfPages * 4096, MMIO_FLAGS);
        }
    }

    serial::print("  total: ");
    serial::print_dec(total_mem_B / 1024);
    serial::print(" KB\n");

    serial::print("   free: ");
    serial::print_dec(free_mem_B / 1024);
    serial::print(" KB\n");

    serial::print("   used: ");
    serial::print_dec((total_mem_B - free_mem_B) / 1024);
    serial::print(" KB\n");

    if (!trampoline_memory_is_free) {
        serial::print("abort: memory used by trampoline not free\n");
        panic(0x00'00'00'ff); // blue
    }

    // map apic registers for interrupt handling
    map_range(uptr(apic.io), 0x1000, MMIO_FLAGS);
    map_range(uptr(apic.local), 0x1000, MMIO_FLAGS);

    // map frame buffer with write-combining (pat index 4)
    auto constexpr static FB_FLAGS = PAGE_P | PAGE_RW | USE_PAT_WC;
    map_range(uptr(frame_buffer.pixels),
              frame_buffer.stride * frame_buffer.height * sizeof(u32),
              FB_FLAGS);

    // map the heap
    map_range(heap_start, heap_size, RAM_FLAGS);

    // config pat: set pa4 to write-combining (0x01)
    // msr 0x277: ia32_pat register
    // rdmsr: read 64-bit model specific register into edx:eax
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277));

    // pat entry 4 occupies bits 32–39 (low byte of high dword)
    // clear pat4 then set to 0x01 (wc)
    // note: wc is essential for framebuffers; it buffers writes to the gpu
    high = (high & ~0xffu) | 1;

    // wrmsr: write edx:eax back to ia32_pat
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(0x277));

    // pat (page attribute table) configured before cr3 activation; no cache
    // flush required

    // activate the new tables
    asm volatile("mov %0, %%cr3" : : "r"(long_mode_pml4) : "memory");
}

auto apic_ticks_per_sec = 0ul;
auto tsc_ticks_per_sec = 0ull;

// reads the 64-bit time stamp counter (tsc)
auto inline read_tsc() -> u64 {
    u32 low;
    u32 high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (u64(high) << 32) | low;
}

// apic timer calibration
auto inline calibrate_apic_and_tsc() -> void {
    // program pit channel 0 for mode 0 (interrupt on terminal count)
    // base freq = 1193182 hz; 10ms ≈ 11931 ticks (0x2e9b)
    outb(0x43, 0x30); // 00(ch0) 11(lo/hi) 000(mode0) 0(binary)

    // load initial count (lsb then msb) per 8254 programming sequence
    outb(0x40, 0x9b); // divisor low byte
    outb(0x40, 0x2e); // divisor high byte

    // lapic initial count register (0x380): set to max
    // timer begins counting down immediately
    apic.local[0x380 / 4] = 0xffff'ffff; // max count

    // capture tsc before the 10ms polling window
    auto const tsc_start = read_tsc();

    // polling pit status via read-back command (0xe2)
    // bit 7 is set when the pit terminal count is reached (10ms elapsed)
    while (true) {
        outb(0x43, 0xe2);       // pit read-back: latch status of channel 0
        if (inb(0x40) & 0x80) { // bit 7 set when terminal count reached
            break;              // 10ms elapsed
        }
    }

    // capture tsc after 10ms
    auto const tsc_end = read_tsc();

    // lapic current count register (0x390): read remaining ticks
    auto const current_count = apic.local[0x390 / 4];

    apic_ticks_per_sec = (0xffff'ffff - current_count) * 100;

    tsc_ticks_per_sec = (tsc_end - tsc_start) * 100;
}

auto constexpr static TIMER_VECTOR = 32u;

// disables legacy pic and starts lapic timer in periodic mode
auto inline init_timer() -> void {
    // disable legacy pic: mask all interrupts on master (0x21) and slave (0xa1)
    // essential to prevent spurious interrupts from deprecated hardware
    outb(0x21, 0xff);
    outb(0xa1, 0xff);

    // svr (spurious interrupt vector register): software enable lapic
    // 0x1ff: set bit 8 (apic software enable) and bits 0-7 (vector 255)
    apic.local[0x0f0 / 4] = 0x1ff;

    // dcr (divide configuration register): set timer divisor
    // 0x03: divide by 16 (timer decrements every 16 bus cycles)
    apic.local[0x3e0 / 4] = 3;

    // lvt timer register: configure mode and vector
    // bit 17 (1 << 17): periodic mode (auto-reloads count)
    // bits 0-7: vector index in idt for timer interrupts
    apic.local[0x320 / 4] = (1 << 17) | TIMER_VECTOR;

    calibrate_apic_and_tsc();

    // icr (initial count register): set the countdown start value
    // use calibration to determine value
    apic.local[0x380 / 4] = apic_ticks_per_sec / config::TIMER_FREQUENCY_HZ;
}

// io-apic register access
// writes to an io-apic register using the index/data window
auto io_apic_write(u32 const reg, u32 const val) -> void {
    // ioregsel (offset 0x00): select the target register index
    apic.io[0x000 / 4] = reg;

    // iowin (offset 0x10): write the 32-bit data to the selected register
    apic.io[0x010 / 4] = val; // write value
}

auto constexpr static KEYBOARD_VECTOR = 33u;

// keyboard and io-apic routing
// routes keyboard irq through io-apic and enables scanning
auto inline init_keyboard() -> void {
    // get local apic id of the current cpu (bits 24-31 of offset 0x020)
    auto const cpu_id = (apic.local[0x020 / 4] >> 24) & 0xff;

    // configure io-apic redirection table for keyboard (usually gsi 1)
    // index 0x10 is the start of the redirection table with 2 x 32-bit
    // registers per entry
    // low 32 bits: vector | flags (trigger mode, polarity, etc.)
    io_apic_write(0x10 + keyboard_config.gsi * 2,
                  KEYBOARD_VECTOR | keyboard_config.flags);
    // high 32 bits: destination field (sets which cpu receives the interrupt)
    io_apic_write(0x10 + keyboard_config.gsi * 2 + 1, cpu_id << 24);

    // flush: clear the output buffer (port 0x60) of any stale data
    // check status register (port 0x64) bit 0 (output buffer full)
    while (inb(0x64) & 1) {
        inb(0x60);
    }

    // wait for controller: check bit 1 (input buffer full)
    // cannot send commands until this bit is 0
    while (inb(0x64) & 2) {
        core::pause();
    }

    // send command 0xf4: enable scanning
    // tells the keyboard to start sending scancodes when keys are pressed
    outb(0x60, 0xf4);

    // diagnostic: wait for 0xfa (acknowledge) from the keyboard
    // assuming hardware is correct, we block indefinitely until ack
    while (true) {
        if (inb(0x64) & 1) {
            auto const response = inb(0x60);
            if (response == 0xfa) {
                serial::print("  ack\n");
                break;
            }
        }
        core::pause();
    }
}

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

// idtr: the 10-byte structure passed to 'lidt'
struct [[gnu::packed]] IDTR {
    u16 limit;
    u64 base;
};

// idt (interrupt descriptor table) init for bootstrap processor
auto inline init_idt_bsp() -> void {
    // alignas(16): required for performance and hardware consistency
    alignas(16) static IDTEntry idt[256];

    // set idt entry lapic timer
    // 0x8e: 10001110b -> p=1, dpl=00, type=1110 (64-bit interrupt gate)
    // p : present
    // dpl: ring 0
    // type: 64-bit interrupt gate (clears IF on entry, preventing nesting)
    // 8: second entry in the gdt (code)
    auto const apic_addr = u64(kernel_asm_timer_handler);
    idt[TIMER_VECTOR] = {u16(apic_addr),       8, 0, 0x8e, u16(apic_addr >> 16),
                         u32(apic_addr >> 32), 0};

    // set idt entry keyboard
    auto const kbd_addr = u64(kernel_asm_keyboard_handler);
    idt[KEYBOARD_VECTOR] = {
        u16(kbd_addr), 8, 0, 0x8e, u16(kbd_addr >> 16), u32(kbd_addr >> 32), 0};

    auto const idtr = IDTR{sizeof(idt) - 1, u64(idt)};

    // lidt: load the interrupt descriptor table register
    asm volatile("lidt %0" : : "m"(idtr));
}

// idt (interrupt descriptor table) init for application processor
auto inline init_idt_ap() -> void {
    alignas(16) static IDTEntry idt[256];
    // empty idt: any interrupt will cause triple fault and reset

    auto const idtr = IDTR{sizeof(idt) - 1, u64(idt)};
    asm volatile("lidt %0" : : "m"(idtr));
}

// keyboard interrupt handler
// c-linkage handler called by assembly isr stub
extern "C" auto kernel_on_keyboard() -> void {
    // drain ps/2 output buffer: bit 0 of status (0x64) means data is waiting
    // reading all pending bytes prevents the controller from getting "stuck"
    while (inb(0x64) & 1) {
        // read raw byte from data port
        auto const scancode = inb(0x60);

        // log scancode to serial for debugging
        serial::print("|");
        serial::print_hex_byte(scancode);
        serial::print("|");

        // notify the os layer that a keyboard event has occured
        osca::on_keyboard(scancode);
    }

    // write any value (conventionally 0) to EOI register
    // notifies the lapic that the handler is finished so it can deliver the
    // next interrupt
    apic.local[0x0b0 / 4] = 0;
}

// lapic timer interrupt handler
// c-linkage handler called by the assembly timer stub
extern "C" auto kernel_on_timer() -> void {
    // notify the os layer that a tick has occurred
    osca::on_timer();

    // write any value (conventionally 0) to EOI register
    // notifies the lapic that the handler is finished so it can deliver the
    // next interrupt
    apic.local[0x0b0 / 4] = 0;
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
    // point a call occurs; since a call pushes an 8-byte return address, the
    // compiler expects the stack to end in 0x8 upon entering a function.

    // the compiler is informed that this point is never reached
    __builtin_unreachable();
}

// flag used by ap to message bsp that ap has started when started sequentially
u8 static run_core_started_flag;

// this is the entry point for application processors
// each core lands here after the trampoline finishes
[[noreturn]] auto run_core() -> void {
    // flag bsp that core is running
    // (1) paired with acquire (2)
    atomic::store(&run_core_started_flag, u8(1), atomic::RELEASE);

    init_fpu();
    init_gdt();
    init_idt_ap();

    // find this core index
    auto const apic_id = (apic.local[0x020 / 4] >> 24) & 0xff;
    for (auto i = 0u; i < core_count; ++i) {
        if (cores[i].apic_id == apic_id) {
            osca::run_core(i);
        }
    }

    // core not found
    panic(0x00'ff'ff'ff); // white
}

auto delay_us(u64 const us) -> void {
    auto const target = read_tsc() + (tsc_ticks_per_sec * us / 1'000'000);
    while (read_tsc() < target) {
        core::pause();
    }
}

auto inline send_init_sipi(u8 const apic_id, u32 const trampoline_address)
    -> void {

    // select target core via high dword of icr
    apic.local[0x310 / 4] = u32(apic_id) << 24;

    // send init ipi to reset the application processor (ap)
    apic.local[0x300 / 4] = 0x00004500;

    // wait until the delivery status bit clears
    while (apic.local[0x300 / 4] & (1 << 12)) {
        core::pause();
    }

    // wait 10ms for ap to settle after reset (intel requirement)
    delay_us(10 * 1'000);

    // convert address to 4KB page vector; 0x8000 -> 0x08
    auto const vector = trampoline_address >> 12;

    // re-select target apic id
    apic.local[0x310 / 4] = u32(apic_id) << 24;

    // send first sipi to wake ap at vector address
    apic.local[0x300 / 4] = 0x00004600 | vector;

    // wait for delivery check
    while (apic.local[0x300 / 4] & (1 << 12)) {
        core::pause();
    }

    // 200us delay before retry (intel requirement)
    delay_us(200);

    // re-select target apic id (intel requirement)
    apic.local[0x310 / 4] = u32(apic_id) << 24;

    // send second sipi (intel requirement)
    apic.local[0x300 / 4] = 0x00004600 | vector;

    // final delivery check
    while (apic.local[0x300 / 4] & (1 << 12)) {
        core::pause();
    }
}

// addresses in the assembler code
extern "C" u8 kernel_asm_run_core_start[];
extern "C" u8 kernel_asm_run_core_end[];
extern "C" u8 kernel_asm_run_core_config[];

auto constexpr static TRAMPOLINE_DEST = uptr(0x8000);

auto inline init_cores() {

    // critical addresses:
    // 0x0'8000 - ?       : start core trampoline code
    // 0x1'0000 - 0x1'1fff: start core pdpt for protected mode code
    //
    // note: address range is checked to be available as conventional memory in
    //       `init_paging` and after cores have launched the memory can be
    //       reclaimed

    // the pages used in trampoline to transition from real -> protected -> long
    auto* const protected_mode_pdpt = ptr<u64>(0x1'0000);
    auto* const protected_mode_pd = ptr<u64>(0x1'1000);

    memset(protected_mode_pdpt, 0, 4096);
    memset(protected_mode_pd, 0, 4096);

    // identity map the first 2MB covering 0x8000, 0x1'0000 -> 0x1'2000
    protected_mode_pdpt[0] = 0x1'1000 | PAGE_P;
    protected_mode_pd[0] = 0 | PAGE_P | PAGE_RW | PAGE_PS;

    // note: page tables are in wb cacheable ram
    //       x86 cache coherence guarantees visibility to ap
    //       no cache flushes or fences required

    serial::print("  count: ");
    serial::print_dec(core_count);
    serial::print("\n");

    // prepare the trampoline with the target function
    // calculate size using the addresses of the labels
    auto const start_addr = uptr(kernel_asm_run_core_start);
    auto const code_size = uptr(kernel_asm_run_core_end) - start_addr;

    // copy the trampoline code to lower 1MB so real mode can run it
    memcpy(ptr<void>(TRAMPOLINE_DEST), kernel_asm_run_core_start, code_size);

    // calculate the offset of the config data relative to the start
    auto const config_offset = uptr(kernel_asm_run_core_config) - start_addr;

    for (auto i = 0u; i < core_count; ++i) {
        // skip the bsp (the core currently running this code)
        // usually the bsp has apic id 0, but check specifically
        auto const bsp_id = apic.local[0x020 / 4] >> 24;
        if (cores[i].apic_id == bsp_id) {
            continue;
        }

        // allocate a unique stack for this specific core
        auto const stack = allocate_pages(config::CORE_STACK_SIZE_PAGES);
        auto const stack_top =
            uptr(stack) + config::CORE_STACK_SIZE_PAGES * 4096;

        // define struct
        struct [[gnu::packed]] TrampolineConfig {
            uptr protected_mode_pdpt;
            uptr stack;
            uptr task;
            uptr long_mode_pml4;
        };
        auto* const config =
            ptr_offset<TrampolineConfig>(TRAMPOLINE_DEST, config_offset);

        // fill the values
        config->protected_mode_pdpt = uptr(protected_mode_pdpt);
        config->stack = uptr(stack_top);
        config->task = uptr(run_core);
        config->long_mode_pml4 = uptr(long_mode_pml4);

        // the core sets flag to 1 once it has started
        run_core_started_flag = 0;

        // send the init-sipi-sipi sequence via the apic to start the core
        send_init_sipi(cores[i].apic_id, TRAMPOLINE_DEST);

        // wait for core to start
        // (2) paired with release (1)
        while (atomic::load(&run_core_started_flag, atomic::ACQUIRE) == 0) {
            core::pause();
        }
    }
}

} // namespace

namespace kernel {

// bump allocator returning zeroed 4KB pages
auto allocate_pages(u64 const num_pages) -> void* {
    auto const bytes = num_pages * 4096;

    // ensure heap has enough space for the requested allocation
    if (heap.size < bytes) {
        serial::print("error: out of memory when allocating pages\n");
        panic(0x00'ff'00'00); // red
    }

    auto* const p = heap.start;
    heap.start = ptr_offset<void>(heap.start, bytes);
    heap.size -= bytes;
    memset(p, 0, bytes);
    return p;
}

} // namespace kernel

[[noreturn]] auto kernel::start() -> void {
    init_serial();
    serial::print("serial initiated\n");

    heap = make_heap();

    serial::print("init_fpu\n");
    init_fpu();

    serial::print("init_gdt\n");
    init_gdt();

    serial::print("init_paging\n");
    init_paging();

    serial::print("init_idt_bsp\n");
    init_idt_bsp();

    serial::print("init_timer\n");
    init_timer();

    serial::print("init_keyboard\n");
    init_keyboard();

    serial::print("init_cores\n");
    init_cores();

    serial::print("osca_start\n");
    osca_start();
}

// required by msvc/clang abi when floating-point arithmetic is used
extern "C" i32 _fltused = 0;
