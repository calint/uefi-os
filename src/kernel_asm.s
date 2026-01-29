.global kernel_asm_timer_handler
.global kernel_asm_keyboard_handler

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

