.intel_syntax noprefix
#ifdef ARCH_X86_64

.code64
#include "common.S"

.extern __syscall_handler

.section .ktext
.global asm_syscall_entry

asm_syscall_entry:
    # Comment this out to take the iret path
    jmp _ignored_iret_construction_path

    # Construct an interrupt frame on stack
    push	USER_DS     # regs.hwframe->ss
    push    rsp         # regs.hwframe->rsp
    push	r11         # regs.hwframe->rflags
    push	USER_CS     # regs.hwframe->cs
    push	rcx         # regs.hwframe->rip

_ignored_iret_construction_path:
    # Save all registers we'll modify
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    # Save syscall number and arguments
    mov rcx, rax  # Save syscall number
    mov r11, rdi  # Save arg1
    mov r12, rsi  # Save arg2
    mov r13, rdx  # Save arg3
    mov r14, r10  # Save arg4
    mov r15, r8   # Save arg5
    mov r10, r9   # Save arg6

    # Move syscall arguments into appropriate registers for __syscall_handler
    mov rdi, rcx  # syscall number
    mov rsi, r11  # arg1
    mov rdx, r12  # arg2
    mov rcx, r13  # arg3
    mov r8,  r14  # arg4
    mov r9,  r15  # arg5
    push r10      # arg6 (7th param goes on the stack according to the System V AMD64 ABI calling convention)

    # Call the C function
    call __syscall_handler

    # Restore all registers in reverse order
    add rsp, 8  # Clean arg6 which was pushed onto the stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi

    #
    # Check if the process is elevated, and if so, take a special retq path.
    #
    push rax                                # preserve syscall return value
    
    # Inline elevation check using rax
    this_cpu_read rax, current_process_core
    mov rax, [rax + 0xd0]                   # current_process_core->hw_state.elevated
    testb al, 1                             # test the elevated bit

    pop rax                                 # restore syscall return value

    # Take a retq path if we are elevated
    jnz	__syscall_exit_swapgs_and_elevated_ret

    # Uncomment this to take the iret path
    # jmp __syscall_exit_swapgs_and_iret

__syscall_exit_swapgs_and_sysret:
    # Switch back to user gs
    swapgs

    sysretq

__syscall_exit_swapgs_and_iret:
    # Switch back to user gs
    swapgs

    iretq

__syscall_exit_swapgs_and_elevated_ret:
    # Switch back to user gs
    swapgs

    # Restore the flags
    push r11
    popfq

    # Push the return address onto the stack
    push rcx

    # Return with the same privilege level (kernel)
    retq

.section .note.GNU-stack, "", @progbits

#endif // ARCH_X86_64
