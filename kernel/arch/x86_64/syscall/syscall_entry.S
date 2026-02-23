/*
 * x86_64 SYSCALL entry point
 *
 * On SYSCALL:
 *   RCX = return RIP
 *   R11 = return RFLAGS
 *   RAX = syscall number
 *   RDI, RSI, RDX, R10, R8, R9 = arguments (Linux convention)
 *
 * SYSCALL does NOT switch stacks. RSP remains unchanged.
 *
 * Exit path:
 *   - Elevated tasks return via RET (stay Ring 0)
 *   - Non-elevated tasks return via SYSRET (Ring 3)
 */

/*
 * Section flags: "ax" = allocatable + executable, @progbits = contains code.
 * Required for custom sections with Clang/LLD to properly resolve PLT32 relocations.
 */
.section .priv.text, "ax", @progbits
.code64
.intel_syntax noprefix

.include "percpu/percpu_asm.inc"

.extern stlx_syscall_handler
.extern current_task_exec

/* Task flags (must match task_exec_core) */
.set TASK_FLAGS_OFFSET,   0x00
.set TASK_FLAG_ELEVATED,  (1 << 0)

.globl stlx_x86_syscall_entry
.type stlx_x86_syscall_entry, @function
stlx_x86_syscall_entry:
    /* Save callee-saved registers and syscall-clobbered state */
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rcx            /* return RIP */
    push r11            /* return RFLAGS */

    /*
     * Shuffle syscall arguments to C calling convention.
     * Syscall: RAX=num, RDI=arg1, RSI=arg2, RDX=arg3, R10=arg4, R8=arg5, R9=arg6
     * C ABI:   RDI=num, RSI=arg1, RDX=arg2, RCX=arg3, R8=arg4,  R9=arg5, stack=arg6
     */
    push r9             /* arg6 on stack */
    mov r9, r8          /* arg5 */
    mov r8, r10         /* arg4 */
    mov rcx, rdx        /* arg3 */
    mov rdx, rsi        /* arg2 */
    mov rsi, rdi        /* arg1 */
    mov rdi, rax        /* syscall number */

    /* Align stack to 16 bytes (9 pushes = 72 bytes, add 8) */
    sub rsp, 8

    call stlx_syscall_handler

    /* Clean up alignment padding and arg6 */
    add rsp, 16

    /* Restore saved state */
    pop r11             /* return RFLAGS */
    pop rcx             /* return RIP */
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    /* Determine exit path based on elevated flag */
    push rax
    this_cpu_read rax, current_task_exec
    mov eax, dword ptr [rax + TASK_FLAGS_OFFSET]
    test eax, TASK_FLAG_ELEVATED
    pop rax

    jnz .Lsysret_elevated_ret_exit

.Lsysret_exit:
    /* Return to Ring 3. SYSRET loads RIP from RCX, RFLAGS from R11. */
    sysretq

.Lsysret_elevated_ret_exit:
    /* Return staying in Ring 0. Manually restore RFLAGS and return. */
    push r11
    popfq
    push rcx
    ret

.size stlx_x86_syscall_entry, . - stlx_x86_syscall_entry
