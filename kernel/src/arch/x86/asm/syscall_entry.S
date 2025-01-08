.intel_syntax noprefix
#ifdef ARCH_X86_64

.code64
#include "common.S"

.extern __syscall_handler
.extern __check_current_elevate_status

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

    #
    # Check if the process is elevated, and if so, take a special retq path.
    #
    push rax                                # preserve syscall return value
    call __check_current_elevate_status     # check the elevate status
    testb al, 0x1                           # if the result is non-zero, then task is elevated
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
