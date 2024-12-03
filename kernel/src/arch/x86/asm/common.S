.intel_syntax noprefix
#ifdef ARCH_X86_64
.code64

.equ KERNEL_CS,   0x08
.equ USER_CS,     0x33
.equ USER_DS,     0x2b

.equ USER_RPL,    0x03

.section .note.GNU-stack, "", @progbits

#endif // ARCH_X86_64
