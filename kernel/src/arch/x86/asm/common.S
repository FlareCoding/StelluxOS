.intel_syntax noprefix
#ifdef ARCH_X86_64
.code64

.equ KERNEL_CS,   0x08
.equ USER_CS,     0x33
.equ USER_DS,     0x2b

.equ USER_RPL,    0x03

.equ per_cpu_offset_current_task,           0x00
.equ per_cpu_offset_default_kernel_stack,   0x08
.equ per_cpu_offset_current_kernel_stack,   0x10
.equ per_cpu_offset_current_user_stack,     0x18

.section .note.GNU-stack, "", @progbits

#endif // ARCH_X86_64
