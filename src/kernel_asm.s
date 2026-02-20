.global kernel_asm_timer_handler
.global kernel_asm_keyboard_handler
.global kernel_asm_run_core_start
.global kernel_asm_run_core_end
.global kernel_asm_run_core_config

.macro PUSH_ALL
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    # r12 is safe to use as an anchor because the C++ ABI 
    # guarantees the handler will restore it if it touches it
    mov %rsp, %r12

    # alignment for xsave performance
    sub $1024, %rsp
    # clear the lower 6 bits (0x3F)
    and $-64, %rsp

    # xsave mask: 7 (x87 | SSE | AVX)
    mov $7, %eax
    xor %edx, %edx
    xsave (%rsp)
.endm

.macro POP_ALL
    # restore the extended state
    mov $7, %eax
    xor %edx, %edx
    xrstor (%rsp)

    # snap rsp back to the state before alignment/allocation
    mov %r12, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
.endm

kernel_asm_timer_handler:
    PUSH_ALL
    cld
    call kernel_on_timer
    POP_ALL
    iretq

kernel_asm_keyboard_handler:
    PUSH_ALL
    cld
    call kernel_on_keyboard
    POP_ALL
    iretq

//
// used by kernel to launch code on a core 
//

.code16
kernel_asm_run_core_start:
.entry:
    .set TRAMPOLINE_BASE, 0x8000
    .set CONFIG_OFFSET, kernel_asm_run_core_config - kernel_asm_run_core_start

    cli
    xorw %ax, %ax
    movw %ax, %ds          # data segment to 0

    # bx acts as base pointer to trampoline location (code of this function)
    movw $TRAMPOLINE_BASE, %bx

    # calculate 16-bit offset of the gdt pointer relative to ds (which is 0)
    movw $(.gdt_ptr - .entry), %si
    addw %bx, %si          # si = 0x8000 + offset
    lgdt (%si)             # load global descriptor table

    # enter protected mode
    movl %cr0, %eax
    orl  $1, %eax          # set bit 0: protected mode enable (pe)
    movl %eax, %cr0

    # jump to 32-bit protected mode using 2'nd entry in gdt
    ljmp $0x08, $(TRAMPOLINE_BASE + (.protected_mode - .entry))

.code32
.protected_mode:
    # initiate segment register with data segment
    movw $0x10, %ax        # 0x10 is 32-bit data selector
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    # access the config struct via esi
    movl $(TRAMPOLINE_BASE + CONFIG_OFFSET), %esi

    # enable physical address extension
    movl %cr4, %eax
    orl  $(1 << 5), %eax   # set bit 5: physical address extension (pae)
    movl %eax, %cr4

    # load bridge page table
    movl 0(%esi), %eax     # 0 = offset of protected_mode_pdpt
    movl %eax, %cr3

    # enable long mode
    movl $0xc0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax   # set bit 8: long mode enable (lme)
    wrmsr

    # enable paging
    movl %cr0, %eax
    orl  $(1 << 31), %eax  # set bit 31: paging (pg)
    movl %eax, %cr0

    # jump to 64-bit long mode using 4'th entry in gdt
    ljmp $0x18, $(TRAMPOLINE_BASE + (.long_mode - .entry))

.code64
.long_mode:
    # point to the config struct
    movq $(TRAMPOLINE_BASE + CONFIG_OFFSET), %rsi
 
    # enable final paging
    # note: currently using temporary paging at 0x1'0000 
    movq 24(%rsi), %rax   # 24 = offset of long_mode_pml4
    movq %rax, %cr3

    # setup segments
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es

    # setup stack
    movq 8(%rsi), %rsp    # 8 = offset of stack pointer
    movq %rsp, %rbp

    # call target
    movq 16(%rsi), %rax   # 16 = offset of call target pointer
    call *%rax            # calling no return task

.align 16
.gdt:
    .quad 0x0000000000000000 # null
    .quad 0x00cf9a000000ffff # 0x08: 32-bit code (for protected_mode)
    .quad 0x00cf92000000ffff # 0x10: 32-bit data
    .quad 0x00af9a000000ffff # 0x18: 64-bit code (l-bit set for long_mode_entry)

.gdt_ptr:
    .word . - .gdt - 1
    .long TRAMPOLINE_BASE + (.gdt - .entry)

.align 16
kernel_asm_run_core_config:
    .fill 32, 1, 0

kernel_asm_run_core_end:
