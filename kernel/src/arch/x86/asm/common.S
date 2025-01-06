.intel_syntax noprefix
#ifdef ARCH_X86_64
.code64

.equ KERNEL_CS,   0x08
.equ USER_CS,     0x33
.equ USER_DS,     0x2b

.equ USER_RPL,    0x03

.macro this_cpu_read result:req, variable:req
    # Calculate the per-CPU offset
    lea rax, \variable
    sub rax, offset __per_cpu_start

    # Read the per-CPU variable using the GS segment
    mov \result, qword ptr gs:[rax]
.endm

.section .note.GNU-stack, "", @progbits

#endif // ARCH_X86_64
