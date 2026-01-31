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

    # create space for FXSAVE (512 bytes, 16-byte aligned)
    # note: stack is now 16 bytes aligned
    # ((ss, rsp, rflags, cs, rip) + 15) * 8
    sub $512, %rsp
    fxsave (%rsp)
.endm

.macro POP_ALL
    fxrstor (%rsp)
    add $512, %rsp

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
    movw %ax, %ds

    # bx acts as our base pointer
    movw $TRAMPOLINE_BASE, %bx

    # calculate 16-bit offset of the gdt pointer relative to ds (which is 0)
    movw $(.early_gdt_ptr - .entry), %si
    addw %bx, %si  # bx is 0x8000
    lgdt (%si)     # load gdt using the absolute address 0x8000 + offset

    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0

    # jump to 32-bit protected mode
    ljmp $0x08, $(TRAMPOLINE_BASE + (.protected_mode - .entry))

.code32
.protected_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    # access the config struct via esi
    movl $(TRAMPOLINE_BASE + CONFIG_OFFSET), %esi

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
    ljmp $0x18, $(TRAMPOLINE_BASE + (.long_mode - .entry))

.code64
.long_mode:
    # point to the config struct
    movq $(TRAMPOLINE_BASE + CONFIG_OFFSET), %rsi
  
    # currently using bridge paging at 0x1'0000 
    # switch to the real kernel tables
    movq 24(%rsi), %rax   # 24 = offset of final_pml4
    movq %rax, %cr3

    # setup segments
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es

    # setup stack
    movq 8(%rsi), %rsp    # 8 = offset of stack pointer
    movq %rsp, %rbp

    # call target
    movq 16(%rsi), %rax   # 16 = offset of target pointer
    call *%rax

.halt:
    hlt
    jmp .halt

.align 16
.early_gdt:
    .quad 0x0000000000000000 # null
    .quad 0x00cf9a000000ffff # 0x08: 32-bit code (for protected_mode)
    .quad 0x00cf92000000ffff # 0x10: 32-bit data
    .quad 0x00af9a000000ffff # 0x18: 64-bit code (L-bit set for long_mode_entry)

.early_gdt_ptr:
    .word . - .early_gdt - 1
    .long TRAMPOLINE_BASE + (.early_gdt - .entry)

.align 16
kernel_asm_run_core_config:
    .fill 32, 1, 0

kernel_asm_run_core_end:
