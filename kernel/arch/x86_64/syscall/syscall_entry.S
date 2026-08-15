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
 * Technique: stash R14/R15 in fixed per-CPU scratch slots to free
 * them, read system_stack_top, write the handoff record to the
 * system stack, then switch RSP. The caller's stack is never touched
 * because it may already be unmapped when the syscall is issued.
 *
 * The saved region below system_stack_top is described by
 * x86::syscall_frame (syscall_frame.h), keep the layout in sync.
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

.set TASK_FLAGS_OFFSET,        0x00
.set TASK_SYS_STACK_OFFSET,    0x10
.set TASK_IRET_PENDING_OFFSET, 0x2E8
.set TASK_IRET_CTX_OFFSET,     0x2F0
.set TASK_FLAG_ELEVATED,       (1 << 0)

/* Fixed per-CPU scratch offsets, verified by percpu::init_bsp */
.set PERCPU_SYSCALL_SCRATCH0,  0x08
.set PERCPU_SYSCALL_SCRATCH1,  0x10

/* thread_cpu_context offsets (sched/thread_cpu_context.h) */
.set CTX_RAX,    0x00
.set CTX_RBX,    0x08
.set CTX_RCX,    0x10
.set CTX_RDX,    0x18
.set CTX_RSI,    0x20
.set CTX_RDI,    0x28
.set CTX_RBP,    0x30
.set CTX_RSP,    0x38
.set CTX_R8,     0x40
.set CTX_R9,     0x48
.set CTX_R10,    0x50
.set CTX_R11,    0x58
.set CTX_R12,    0x60
.set CTX_R13,    0x68
.set CTX_R14,    0x70
.set CTX_R15,    0x78
.set CTX_RIP,    0x80
.set CTX_RFLAGS, 0x88
.set CTX_CS,     0x90
.set CTX_SS,     0x98

.globl stlx_x86_syscall_entry
.type stlx_x86_syscall_entry, @function
stlx_x86_syscall_entry:
    swapgs
    lfence

    /* Free R14/R15 via per-CPU scratch, never via the caller's stack. */
    mov qword ptr gs:[PERCPU_SYSCALL_SCRATCH0], r14
    mov qword ptr gs:[PERCPU_SYSCALL_SCRATCH1], r15

    /* Preserve syscall number while RAX is reused for per-CPU access. */
    mov r14, rax

    /* Read system_stack_top from current task's exec core. */
    this_cpu_read rax, current_task_exec
    mov r15, qword ptr [rax + TASK_SYS_STACK_OFFSET]

    /*
     * Write handoff record to top of system stack:
     *   [S - 8]  = original caller RSP
     *   [S - 16] = original R14
     *   [S - 24] = original R15
     */
    mov qword ptr [r15 - 8], rsp
    mov rax, qword ptr gs:[PERCPU_SYSCALL_SCRATCH0]
    mov qword ptr [r15 - 16], rax
    mov rax, qword ptr gs:[PERCPU_SYSCALL_SCRATCH1]
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

    /*
     * A staged full-register context (rt_sigreturn of a frame captured
     * outside a syscall) replaces the frame-based return entirely:
     * SYSRET consumes RCX/R11, only IRET can rebuild every register.
     */
    push rax
    this_cpu_read rax, current_task_exec
    cmp dword ptr [rax + TASK_IRET_PENDING_OFFSET], 0
    pop rax
    jne .Lsyscall_iret_exit

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

.Lsyscall_iret_exit:
    /*
     * Full-register return: the staged context was sanitized by the
     * rt_sigreturn path (user segments forced, RFLAGS masked, RIP
     * bounded), so IRET always lands back in user mode.
     */
    this_cpu_read rax, current_task_exec
    mov dword ptr [rax + TASK_IRET_PENDING_OFFSET], 0
    lea rax, [rax + TASK_IRET_CTX_OFFSET]

    /* Hardware return frame from the staged context. */
    push qword ptr [rax + CTX_SS]
    push qword ptr [rax + CTX_RSP]
    push qword ptr [rax + CTX_RFLAGS]
    push qword ptr [rax + CTX_CS]
    push qword ptr [rax + CTX_RIP]

    /* Every general register from the staged context, RAX last. */
    mov rbx, [rax + CTX_RBX]
    mov rcx, [rax + CTX_RCX]
    mov rdx, [rax + CTX_RDX]
    mov rsi, [rax + CTX_RSI]
    mov rdi, [rax + CTX_RDI]
    mov rbp, [rax + CTX_RBP]
    mov r8,  [rax + CTX_R8]
    mov r9,  [rax + CTX_R9]
    mov r10, [rax + CTX_R10]
    mov r11, [rax + CTX_R11]
    mov r12, [rax + CTX_R12]
    mov r13, [rax + CTX_R13]
    mov r14, [rax + CTX_R14]
    mov r15, [rax + CTX_R15]
    mov rax, [rax + CTX_RAX]

    swapgs
    iretq

.size stlx_x86_syscall_entry, . - stlx_x86_syscall_entry
