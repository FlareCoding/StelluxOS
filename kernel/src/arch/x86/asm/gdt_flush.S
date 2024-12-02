.intel_syntax noprefix
#ifdef ARCH_X86_64

.code64

.section .ktext

.global asm_flush_gdt

asm_flush_gdt:
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

#endif // ARCH_X86_64
