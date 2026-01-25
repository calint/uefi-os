.global kernel_load_gdt
.global kernel_apic_timer_handler
.global kernel_keyboard_handler
.global osca_start

kernel_load_gdt:
    lgdt (%RCX)              # rcx has descriptor address
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

kernel_apic_timer_handler:
    push %RAX
    push %RCX
    push %RDX
    push %RSI
    push %RDI
    push %R8
    push %R9
    push %R10
    push %R11

    cld                      # ensure string ops go forward

    # stack alignment check: (5 hardware + 9 manual) = 14 qwords
    # 14 * 8 = 112 bytes. 112 is not a multiple of 16.
    sub $8, %RSP             # align stack to 16 bytes
    call osca_apic_timer_handler
    add $8, %RSP             # restore alignment

    pop %R11
    pop %R10
    pop %R9
    pop %R8
    pop %RDI
    pop %RSI
    pop %RDX
    pop %RCX
    pop %RAX
    iretq

kernel_keyboard_handler:
    push %RAX
    push %RCX
    push %RDX
    push %RSI
    push %RDI
    push %R8
    push %R9
    push %R10
    push %R11

    cld                      # ensure string ops go forward

    # stack alignment check: (5 hardware + 9 manual) = 14 qwords
    # 14 * 8 = 112 bytes. 112 is not a multiple of 16.
    sub $8, %RSP             # align stack to 16 bytes
    call osca_keyboard_handler
    add $8, %RSP             # restore alignment

    pop %R11
    pop %R10
    pop %R9
    pop %R8
    pop %RDI
    pop %RSI
    pop %RDX
    pop %RCX
    pop %RAX
    iretq
