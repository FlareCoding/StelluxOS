.intel_syntax noprefix
.section .text
.global _stlx_crt_start

.equ SYSCALL_SYS_EXIT, 2

.extern main

#
# x86_64 SysV ABI
#
_stlx_crt_start:
   call main

   # Put return value from main into RDI for the exit system call
   mov rdi, rax

   # Exit system call
   mov rax, SYSCALL_SYS_EXIT
   syscall

   # The syscall should never return in a normal scenario.
   hlt

.section .note.GNU-stack, "", @progbits
