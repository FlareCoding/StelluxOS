/*
 * x86_64 Layer 1 trap stubs.
 *
 * Stack layout on entry to stlx_x86_common_entry after per-vector normalization:
 *   0x00: vector
 *   0x08: error_code (0 for vectors without a hardware error code)
 *   0x10: RIP
 *   0x18: CS
 *   0x20: RFLAGS
 *   0x28: RSP
 *   0x30: SS
 */

.section .priv.text, "ax", @progbits
.code64

.intel_syntax noprefix

.include "percpu/percpu_asm.inc"

.extern stlx_x86_64_trap_handler
.extern current_task

.section .priv.rodata, "a", @progbits
.p2align 3
.globl stlx_x86_isr_table
.type stlx_x86_isr_table, @object
stlx_x86_isr_table:
.section .priv.text, "ax", @progbits

/* Normalized entry frame offsets (relative to rsp at common entry). */
.set HDR_VECTOR, 0x00
.set HDR_ERROR,  0x08
.set HDR_RIP,    0x10
.set HDR_CS,     0x18
.set HDR_RFLAGS, 0x20
.set HDR_RSP,    0x28
.set HDR_SS,     0x30

/* trap::trap_frame offsets (relative to trap_frame base). */
.set TF_R15,   0x00
.set TF_R14,   0x08
.set TF_R13,   0x10
.set TF_R12,   0x18
.set TF_R11,   0x20
.set TF_R10,   0x28
.set TF_R9,    0x30
.set TF_R8,    0x38
.set TF_RBP,   0x40
.set TF_RDI,   0x48
.set TF_RSI,   0x50
.set TF_RDX,   0x58
.set TF_RCX,   0x60
.set TF_RBX,   0x68
.set TF_RAX,   0x70

.set TF_VECTOR, 0x78
.set TF_ERROR,  0x80
.set TF_RIP,    0x88
.set TF_CS,     0x90
.set TF_RFLAGS, 0x98
.set TF_RSP,    0xA0
.set TF_SS,     0xA8
.set TF_SIZE,   0xB0

/* Task structure offsets (from sched/task_exec_core.h)
 * CRITICAL: These must match the C++ struct layout!
 * struct task_exec_core {
 *     uint32_t  flags;           // 0x00
 *     uint32_t  cpu;             // 0x04
 *     uintptr_t task_stack_top;  // 0x08
 *     uintptr_t system_stack_top;// 0x10
 *     thread_cpu_context cpu_ctx;// 0x18
 *     ...
 * };
 */
.set TASK_FLAGS_OFFSET,     0x00
.set TASK_SYS_STACK_OFFSET, 0x10

/* Task flags (must match sched/task_exec_core.h) */
.set TASK_FLAG_ELEVATED, (1 << 0)
.set TASK_FLAG_IN_IRQ,   (1 << 6)

/*
 * Per-vector entry stub generator.
 *
 * Vectors with hardware error codes: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30.
 * All others push a dummy error_code=0.
 *
 * We use \@ to generate the numeric vector value (0..255) across 256 invocations.
 */
.macro STLX_X86_MAKE_ISR
.globl stlx_x86_isr_\@
.type stlx_x86_isr_\@, @function
stlx_x86_isr_\@:
    .pushsection .priv.rodata, "a", @progbits
    .quad stlx_x86_isr_\@
    .popsection
    .if (\@ == 8)
        push \@                   /* vector; CPU already pushed error_code */
    .elseif (\@ == 10)
        push \@
    .elseif (\@ == 11)
        push \@
    .elseif (\@ == 12)
        push \@
    .elseif (\@ == 13)
        push \@
    .elseif (\@ == 14)
        push \@
    .elseif (\@ == 17)
        push \@
    .elseif (\@ == 21)
        push \@
    .elseif (\@ == 29)
        push \@
    .elseif (\@ == 30)
        push \@
    .else
        push 0                    /* error_code */
        push \@                   /* vector */
    .endif
    jmp stlx_x86_common_entry
.endm

.rept 256
STLX_X86_MAKE_ISR
.endr

.type stlx_x86_common_entry, @function
stlx_x86_common_entry:
    cld
    sub rsp, TF_SIZE

    /* Save GPRs to trap_frame. */
    mov [rsp + TF_R15], r15
    mov [rsp + TF_R14], r14
    mov [rsp + TF_R13], r13
    mov [rsp + TF_R12], r12
    mov [rsp + TF_R11], r11
    mov [rsp + TF_R10], r10
    mov [rsp + TF_R9],  r9
    mov [rsp + TF_R8],  r8
    mov [rsp + TF_RBP], rbp
    mov [rsp + TF_RDI], rdi
    mov [rsp + TF_RSI], rsi
    mov [rsp + TF_RDX], rdx
    mov [rsp + TF_RCX], rcx
    mov [rsp + TF_RBX], rbx
    mov [rsp + TF_RAX], rax

    /* Copy iret frame into trap_frame. */
    lea rsi, [rsp + TF_SIZE]
    mov rax, [rsi + HDR_VECTOR]
    mov [rsp + TF_VECTOR], rax
    mov rax, [rsi + HDR_ERROR]
    mov [rsp + TF_ERROR], rax
    mov rax, [rsi + HDR_RIP]
    mov [rsp + TF_RIP], rax
    mov rax, [rsi + HDR_CS]
    mov [rsp + TF_CS], rax
    mov rax, [rsi + HDR_RFLAGS]
    mov [rsp + TF_RFLAGS], rax
    mov rax, [rsi + HDR_RSP]
    mov [rsp + TF_RSP], rax
    mov rax, [rsi + HDR_SS]
    mov [rsp + TF_SS], rax

    /* Branch based on privilege level. */
    test byte ptr [rsp + TF_CS], 3
    jz .Lstlx_x86_from_kernel

.Lstlx_x86_from_user:
    /* swapgs + lfence for Spectre mitigation. */
    swapgs
    lfence
    jmp .Lstlx_x86_call_c

.Lstlx_x86_from_kernel:
    /*
     * Check if this is an elevated task that needs stack switch.
     * Switch only if: ELEVATED=1 AND IN_IRQ=0
     * (If IN_IRQ=1, we're already on system_stack from a previous interrupt.)
     */
    push rax
    push rbx
    
    this_cpu_read rax, current_task
    mov ebx, [rax + TASK_FLAGS_OFFSET]
    and ebx, (TASK_FLAG_ELEVATED | TASK_FLAG_IN_IRQ)
    cmp ebx, TASK_FLAG_ELEVATED
    jne .Lstlx_x86_kernel_no_switch
    
    /*
     * Elevated task - copy frame to system_stack.
     *
     * Stack layout:
     *   [rsp+0x00] rbx, rax (16 bytes)
     *   [rsp+0x10] trap_frame (TF_SIZE bytes)
     *   [rsp+0x10+TF_SIZE] header (56 bytes)
     */
    mov rbx, [rax + TASK_SYS_STACK_OFFSET]
    sub rbx, (16 + TF_SIZE + 56)
    
    /* Copy frame using r8/r9/r10 (saved in trap_frame). */
    mov r8, rsp
    mov r9, rbx
    mov r10, (16 + TF_SIZE + 56) / 8
.Lstlx_x86_copy_frame:
    mov rax, [r8]
    mov [r9], rax
    add r8, 8
    add r9, 8
    dec r10
    jnz .Lstlx_x86_copy_frame
    
    mov rsp, rbx
    pop rbx
    pop rax
    jmp .Lstlx_x86_call_c

.Lstlx_x86_kernel_no_switch:
    pop rbx
    pop rax

.Lstlx_x86_call_c:
    mov rdi, rsp
    mov r12, rsp
    and rsp, -16
    call stlx_x86_64_trap_handler
    mov rsp, r12

    /* Write back trap_frame to iret frame. */
    lea rsi, [rsp + TF_SIZE]
    mov rax, [rsp + TF_RIP]
    mov [rsi + HDR_RIP], rax
    mov rax, [rsp + TF_CS]
    mov [rsi + HDR_CS], rax
    mov rax, [rsp + TF_RFLAGS]
    mov [rsi + HDR_RFLAGS], rax
    mov rax, [rsp + TF_RSP]
    mov [rsi + HDR_RSP], rax
    mov rax, [rsp + TF_SS]
    mov [rsi + HDR_SS], rax

    /* Restore GPRs from trap_frame. */
    mov r15, [rsp + TF_R15]
    mov r14, [rsp + TF_R14]
    mov r13, [rsp + TF_R13]
    mov r12, [rsp + TF_R12]
    mov r11, [rsp + TF_R11]
    mov r10, [rsp + TF_R10]
    mov r9,  [rsp + TF_R9]
    mov r8,  [rsp + TF_R8]
    mov rbp, [rsp + TF_RBP]
    mov rdi, [rsp + TF_RDI]
    mov rsi, [rsp + TF_RSI]
    mov rdx, [rsp + TF_RDX]
    mov rcx, [rsp + TF_RCX]
    mov rbx, [rsp + TF_RBX]
    mov rax, [rsp + TF_RAX]

    add rsp, TF_SIZE

    /* swapgs back if returning to user mode. */
    test byte ptr [rsp + HDR_CS], 3
    jz .Lstlx_x86_no_swapgs_back
    swapgs
    lfence
.Lstlx_x86_no_swapgs_back:

    add rsp, 16
    iretq

.pushsection .priv.rodata, "a", @progbits
.size stlx_x86_isr_table, 2048
.popsection

