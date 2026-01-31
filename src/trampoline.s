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

    # calculate 16-bit offset of the gdt pointer relative to ds (which is 0)
    movw $(early_gdt_ptr - trampoline_start), %si
    addw %bx, %si  # bx is 0x8000
    lgdt (%si)     # load gdt using the absolute address 0x8000 + offset

    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0

    # jump to 32-bit protected mode
    ljmp $0x08, $(TRAMPOLINE_BASE + (protected_mode - trampoline_start))

.code32
protected_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    # access the config struct via esi
    movl $(TRAMPOLINE_BASE + (trampoline_config_data - trampoline_start)), %esi

    # enable pae
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    # load cr3 from the first 4 bytes of our struct
    movl 0(%esi), %eax
    movl %eax, %cr3

    # enable long mode in efer msr
    movl $0xc0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    # enable paging
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    # inside protected_mode, after enabling paging: use selector 0x18 instead of
    # 0x08
    ljmp $0x18, $(TRAMPOLINE_BASE + (long_mode_entry - trampoline_start))

.code64
long_mode_entry:
    # point to the config struct
    movq $(TRAMPOLINE_BASE + (trampoline_config_data - trampoline_start)), %rsi
  
    # We are currently on the Bridge (0x10000). 
    # Now we switch to the REAL kernel tables that map > 4GB.
    movq 24(%rsi), %rax   # 24 = offset of final_pml4
    movq %rax, %cr3       # CPU can now see the high-memory kernel!

    # setup segments and stack
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es
    movq 8(%rsi), %rsp
    movq %rsp, %rbp

    # now the call target
    movq 16(%rsi), %rax
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
    .fill 64, 1, 0

.global trampoline_end
trampoline_end:
