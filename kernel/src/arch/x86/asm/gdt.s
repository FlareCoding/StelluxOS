.intel_syntax noprefix
.code64

.section .ktext

.global __kinstall_gdt_asm

__kinstall_gdt_asm:
    lgdt [rdi]      # GDT will be passed in from C as a parameter

    mov ax, 0x10    # selector for kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    # Get the return address from the stack into rdi
    pop rdi

    # Push kernel code selector onto the stack
    mov rax, 0x08
    push rax

    # Push the return address back onto the stack
    push rdi

    # Perform a far jump
    retfq

.section .note.GNU-stack, "", @progbits
