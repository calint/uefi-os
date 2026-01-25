.global load_gdt
.global switch_stack

load_gdt:
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

switch_stack:
    # RCX = new stack top
    # RDX = address of function to jump to
    mov %RCX, %RSP
    mov %RCX, %RBP

    # jump to the target function manually
    jmp *%RDX
