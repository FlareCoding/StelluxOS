.intel_syntax noprefix
.code64

.extern __syscall_handler

.global __asm_syscall_entry64

.text
__asm_syscall_entry64:
    # Switch to kernel gs
    swapgs

    # Save the user stack in a temp register
    mov r14, rsp

    # Switch to kernel stack
    mov r15, gs:[0x4]  # rsp0 is at offset 0x4 in the TSS
    mov rsp, r15

    # Store the current user stack
    push r14

    # Save volatile registers that we are going to modify
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push r10
    push r11

    # Move syscall arguments into appropriate registers
    mov r9,  r8   # arg5
    mov r8,  r10  # arg4
    mov r10, rdx  # arg3
    mov rdx, rsi  # arg2
    mov rsi, rdi  # arg1
    mov rdi, rax  # syscall number

    # Call the C function
    call __syscall_handler

    # Restore volatile registers
    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi

    # Switch back to user stack
    pop r15
    mov rsp, r15

    // Switch back to user gs
    swapgs

    # Return to caller with SYSRET
    sysretq

.section .note.GNU-stack, "", @progbits
