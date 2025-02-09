.intel_syntax noprefix
.section .text
.global _stlx_crt_start

.equ SYSCALL_SYS_EXIT, 2

.extern main

# Linker-provided symbols marking the start and end of .init_array
.extern __init_array_start
.extern __init_array_end

#
# x86_64 SysV ABI
#
_stlx_crt_start:
   # Allocate space on the stack for the current constructor pointer
   sub rsp, 8

   # Initialize loop by loading __init_array_start into [rsp]
   mov rax, offset __init_array_start
   mov [rsp], rax

init_loop:
   # Load the current constructor function pointer
   mov rax, [rsp]

   # Check if we've reached __init_array_end
   cmp rax, offset __init_array_end
   je end_init_loop

   # Call the constructor
   call [rax]

   # Advance to the next constructor function pointer
   mov rax, [rsp]
   add rax, 8
   mov [rsp], rax

   jmp init_loop

end_init_loop:
   # Clean up the stack
   add rsp, 8

   # Call the application's entry point
   call main

   # Place return value from main into rdi for exit syscall
   mov rdi, rax

   # Exit system call
   mov rax, SYSCALL_SYS_EXIT
   syscall

   # The syscall should never return in a normal scenario
   hlt

.section .note.GNU-stack, "", @progbits
