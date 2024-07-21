.intel_syntax noprefix
.code64

.equ __KERNEL_CS,   0x08
.equ __USER_CS,     0x33
.equ __USER_DS,     0x2b

.equ __USER_RPL,    0x03

.equ per_cpu_offset_current_task,           0x00
.equ per_cpu_offset_default_kernel_stack,   0x08
.equ per_cpu_offset_current_kernel_stack,   0x10
.equ per_cpu_offset_current_user_stack,     0x18
.equ per_cpu_offset_cpuid,                  0x20

.section .note.GNU-stack, "", @progbits
