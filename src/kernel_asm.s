.global kernel_load_gdt
.global kernel_asm_timer_handler
.global kernel_asm_keyboard_handler
.global osca_start

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
    sub $512, %rsp
    fxsave (%rsp)
    # align stack to 16 bytes for C++ calling convention
    # (hardware (40) + GPRs (120) + FXSAVE (512) = 672 bytes. 672 % 16 == 0)
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

kernel_load_gdt:
    lgdt (%RCX)              # rcx has descriptor address

    // RAX is a "Caller-Saved" Register
    mov $0x10, %AX           # data segment
    mov %AX, %DS
    mov %AX, %ES
    mov %AX, %SS

    pushq $0x08              # code segment
    lea .reload_cs(%RIP), %RAX
    pushq %RAX
    lretq
.reload_cs:
    ret

osca_start:
    # RCX = new stack top
    # RDX = address of function to jump to
    mov %RCX, %RSP
    mov %RCX, %RBP

    # jump to the target function manually
    jmp *%RDX

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

