/*
 * x86_64 SYSCALL entry point with kernel stack switch
 *
 * On SYSCALL:
 *   RCX = return RIP
 *   R11 = return RFLAGS
 *   RAX = syscall number
 *   RDI, RSI, RDX, R10, R8, R9 = arguments
 *
 * SYSCALL does NOT switch stacks, so we switch to system_stack_top
 * immediately before any register saves. This ensures the kernel
 * never runs on the user/caller stack.
 *
 * Technique: push R14/R15 to the caller's stack as scratch, read
 * system_stack_top, copy saved values to the system stack, then
 * switch RSP. After the switch, the caller's stack is never
 * touched again.
 *
 * Exit path:
 *   - Elevated tasks return via RET (stay in Ring 0, keep kernel GS)
 *   - Non-elevated tasks return via swapgs + SYSRET (Ring 3)
 */

.section .priv.text, "ax", @progbits
.code64
.intel_syntax noprefix

.include "percpu/percpu_asm.inc"

.extern stlx_syscall_handler
.extern current_task_exec

.set TASK_FLAGS_OFFSET,     0x00
.set TASK_SYS_STACK_OFFSET, 0x10
.set TASK_FLAG_ELEVATED,    (1 << 0)

.globl stlx_x86_syscall_entry
.type stlx_x86_syscall_entry, @function
stlx_x86_syscall_entry:
    swapgs
    lfence

    /* Save R14/R15 to caller's stack to free them as scratch. */
    push r14
    push r15

    /* Preserve syscall number while RAX is reused for per-CPU access. */
    mov r14, rax

    /* Read system_stack_top from current task's exec core. */
    this_cpu_read rax, current_task_exec
    mov r15, qword ptr [rax + TASK_SYS_STACK_OFFSET]

    /* Compute original caller RSP (before our 2 pushes). */
    lea rax, [rsp + 16]

    /*
     * Write handoff record to top of system stack:
     *   [S - 8]  = original caller RSP
     *   [S - 16] = original R14
     *   [S - 24] = original R15
     */
    mov qword ptr [r15 - 8], rax
    mov rax, qword ptr [rsp + 8]
    mov qword ptr [r15 - 16], rax
    mov rax, qword ptr [rsp]
    mov qword ptr [r15 - 24], rax

    /* Switch to system stack. */
    lea rsp, [r15 - 24]

    /* Restore syscall number. */
    mov rax, r14

    /* Save remaining callee-saved regs + return state on system stack. */
    push r13
    push r12
    push rbp
    push rbx
    push rcx            /* return RIP */
    push r11            /* return RFLAGS */

    /*
     * Save user-visible argument registers. Linux preserves these across
     * syscalls and musl libc depends on that behavior (e.g. readdir uses
     * r8 across a getdents64 syscall).
     */
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi

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

    /* 16 qwords from 16-aligned system_stack_top = 128 bytes = aligned. */
    call stlx_syscall_handler

    /* Clean up arg6. */
    add rsp, 8

    /* Restore user argument registers. */
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9

    /* Restore saved state. */
    pop r11             /* return RFLAGS */
    pop rcx             /* return RIP */
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r15             /* original R15 from handoff record */
    pop r14             /* original R14 from handoff record */

    /* Check elevated flag (preserve return value in RAX). */
    push rax
    this_cpu_read rax, current_task_exec
    mov eax, dword ptr [rax + TASK_FLAGS_OFFSET]
    test eax, TASK_FLAG_ELEVATED
    pop rax

    /* Restore original caller RSP (from handoff record). */
    pop rsp

    jnz .Lsyscall_ret_exit

.Lsyscall_sysret_exit:
    swapgs
    lfence
    sysretq

.Lsyscall_ret_exit:
    /* Elevated: stay in Ring 0, keep kernel GS. */
    push r11
    popfq
    push rcx
    ret

.size stlx_x86_syscall_entry, . - stlx_x86_syscall_entry
