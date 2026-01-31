# trampoline.S - AP entry point in AT&T syntax
.section .text
.global trampoline_start
.global trampoline_end
.global trampoline_config_data

.set TRAMPOLINE_BASE, 0x8000

.code16
trampoline_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds

    # bx acts as our base pointer
    movw $TRAMPOLINE_BASE, %bx

    # Calculate 16-bit offset of the GDT pointer relative to DS (which is 0)
    movw $(early_gdt_ptr - trampoline_start), %si
    addw %bx, %si  # bx is 0x8000
    lgdt (%si)     # Load GDT using the absolute address 0x8000 + offset

    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0

    # Far jump to 32-bit Protected Mode. Syntax: ljmp $selector, $offset
    ljmp $0x08, $(TRAMPOLINE_BASE + (protected_mode - trampoline_start))

.code32
protected_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    # Access the config struct via esi
    movl $(TRAMPOLINE_BASE + (trampoline_config_data - trampoline_start)), %esi

    # Enable PAE
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    # Load CR3 from the first 4 bytes of our struct
    movl (%esi), %eax
    movl %eax, %cr3

    # Enable Long Mode in EFER MSR
    movl $0xc0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    # Enable Paging
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    # --- Step 2: Protected Mode Heartbeat ---
    mov $0x3f8, %dx
    mov $0x50, %al  # 'P'
    out %al, %dx
    # --------------------------

    # Inside protected_mode, after enabling paging:
    # Use selector 0x18 instead of 0x08
    ljmp $0x18, $(TRAMPOLINE_BASE + (long_mode_entry - trampoline_start))

.code64
long_mode_entry:
    # Set up data segments for 64-bit
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es

    # Point to the config struct for stack and entry point
    movq $(TRAMPOLINE_BASE + (trampoline_config_data - trampoline_start)), %rsi

    # Load stack from struct (offset 8)
    movq 8(%rsi), %rsp
    movq %rsp, %rbp
    
    # Load C++ entry point from struct (offset 16)
    movq 16(%rsi), %rax

    # Call the C++ function
    call *%rax

.halt:
    hlt
    jmp .halt

.align 16
early_gdt:
    .quad 0x0000000000000000 # null
    .quad 0x00cf9a000000ffff # 0x08: 32-bit code (for protected_mode)
    .quad 0x00cf92000000ffff # 0x10: 32-bit data
    .quad 0x00af9a000000ffff # 0x18: 64-bit code (L-bit set for long_mode_entry)

early_gdt_ptr:
    .word . - early_gdt - 1
    .long TRAMPOLINE_BASE + (early_gdt - trampoline_start)

.align 16
trampoline_config_data:
    .fill 32, 1, 0

.global trampoline_end
trampoline_end:
