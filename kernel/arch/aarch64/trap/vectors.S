/*
 * AArch64 Layer 1 exception vectors (VBAR_EL1).
 *
 * Linux-style layout: 16 slots × 128 bytes, table aligned to 2KB.
 * Each slot branches to an out-of-line handler which builds a trap::trap_frame
 * on the kernel stack and calls into C++.
 */

.section .priv.text, "ax", @progbits

.extern stlx_aarch64_el0_sync_handler
.extern stlx_aarch64_el0_irq_handler
.extern stlx_aarch64_el0_fiq_handler
.extern stlx_aarch64_el0_serror_handler
.extern stlx_aarch64_el1_sync_handler
.extern stlx_aarch64_el1_irq_handler
.extern stlx_aarch64_el1_fiq_handler
.extern stlx_aarch64_el1_serror_handler

/* trap::trap_frame offsets (bytes) */
.set TF_X0,    0x00
.set TF_SP,    0xF8
.set TF_SP_EL1, 0x100
.set TF_ELR,   0x108
.set TF_SPSR,  0x110
.set TF_ESR,   0x118
.set TF_FAR,   0x120
.set TF_SIZE,  0x130

.macro STLX_A64_SAVE_GPRS
    /* ARMv8-A guarantees SP_EL1 is 16-byte aligned on exception entry.
     * TF_SIZE (0x130) is 16-byte aligned, so SP remains aligned after subtraction. */
    sub sp, sp, #TF_SIZE
    stp x0,  x1,  [sp, #0x00]
    stp x2,  x3,  [sp, #0x10]
    stp x4,  x5,  [sp, #0x20]
    stp x6,  x7,  [sp, #0x30]
    stp x8,  x9,  [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x18, x19, [sp, #0x90]
    stp x20, x21, [sp, #0xA0]
    stp x22, x23, [sp, #0xB0]
    stp x24, x25, [sp, #0xC0]
    stp x26, x27, [sp, #0xD0]
    stp x28, x29, [sp, #0xE0]
    str x30, [sp, #0xF0]
.endm

.macro STLX_A64_RESTORE_GPRS_EXCEPT_X16
    ldp x0,  x1,  [sp, #0x00]
    ldp x2,  x3,  [sp, #0x10]
    ldp x4,  x5,  [sp, #0x20]
    ldp x6,  x7,  [sp, #0x30]
    ldp x8,  x9,  [sp, #0x40]
    ldp x10, x11, [sp, #0x50]
    ldp x12, x13, [sp, #0x60]
    ldp x14, x15, [sp, #0x70]
    ldr x17, [sp, #0x88]
    ldp x18, x19, [sp, #0x90]
    ldp x20, x21, [sp, #0xA0]
    ldp x22, x23, [sp, #0xB0]
    ldp x24, x25, [sp, #0xC0]
    ldp x26, x27, [sp, #0xD0]
    ldp x28, x29, [sp, #0xE0]
    ldr x30, [sp, #0xF0]
.endm

.macro STLX_A64_CAPTURE_SYSREGS
    /* Use x16 (IP0, caller-saved scratch) as temporary to avoid clobbering x0.
     * x16 is already saved in trap_frame at offset 0x80, so safe to use here. */
    mrs x16, elr_el1
    str x16, [sp, #TF_ELR]
    mrs x16, spsr_el1
    str x16, [sp, #TF_SPSR]
    mrs x16, esr_el1
    str x16, [sp, #TF_ESR]
    mrs x16, far_el1
    str x16, [sp, #TF_FAR]
.endm

.macro STLX_A64_WRITEBACK_SYSREGS
    /* Use x16 (IP0, caller-saved scratch) as temporary to avoid clobbering x0.
     * x16 will be restored from trap_frame at offset 0x80 in epilogue. */
    ldr x16, [sp, #TF_ELR]
    msr elr_el1, x16
    ldr x16, [sp, #TF_SPSR]
    msr spsr_el1, x16
.endm

.macro STLX_A64_CALL_C handler
    /* SP is 16-byte aligned: ARMv8-A guarantees SP_EL1 alignment on exception entry,
     * and TF_SIZE (0x130 = 304 = 16×19) preserves alignment. AArch64 PCS requires
     * 16-byte alignment at function call boundaries. */
    mov x0, sp                  /* arg0 = trap_frame* */
    bl \handler
.endm

/*
 * Mode-aware trap epilogue:
 *   - Return to EL1h (SPSR.M[0]=1): restore SP_EL1 from trap_frame.sp
 *   - Return to EL0t/EL1t (SPSR.M[0]=0):
 *       * restore SP_EL0 from trap_frame.sp
 *       * switch active EL1 stack to trap_frame.sp_el1
 *
 * This is required when scheduler switches away from an in-kernel blocked
 * context: the outgoing task's kernel call chain must not remain as the
 * active SP_EL1 for unrelated tasks.
 */
.macro STLX_A64_RETURN_FROM_FRAME
    ldr x17, [sp, #TF_SPSR]
    tst x17, #1
    b.ne 1f

    /* Return to EL0t/EL1t: publish selected task SP_EL0. */
    ldr x17, [sp, #TF_SP]
    msr sp_el0, x17

    STLX_A64_WRITEBACK_SYSREGS
    isb
    mov x16, sp
    STLX_A64_RESTORE_GPRS_EXCEPT_X16

    /* Install selected task's exception stack for the next trap. */
    ldr x17, [x16, #TF_SP_EL1]
    mov sp, x17

    /* Restore scratch regs last (x16 held frame base). */
    ldr x17, [x16, #0x88]
    ldr x16, [x16, #0x80]
    eret

1:
    /* Return to EL1h: resume at exact interrupted kernel SP. */
    STLX_A64_WRITEBACK_SYSREGS
    isb
    mov x16, sp
    STLX_A64_RESTORE_GPRS_EXCEPT_X16
    ldr x17, [x16, #TF_SP]
    mov sp, x17

    /* Restore scratch regs last (x16 held frame base). */
    ldr x17, [x16, #0x88]
    ldr x16, [x16, #0x80]
    eret
.endm

/* EL1-origin prologue/epilogue */
.macro STLX_A64_EL1_ENTRY handler
    STLX_A64_SAVE_GPRS

    /* trap_frame.sp = interrupted kernel SP (before allocating trap_frame) */
    /* Use x16 (IP0, caller-saved scratch) as temporary. x16 already saved above. */
    add x16, sp, #TF_SIZE
    str x16, [sp, #TF_SP]
    str x16, [sp, #TF_SP_EL1]

    STLX_A64_CAPTURE_SYSREGS
    STLX_A64_CALL_C \handler
    STLX_A64_RETURN_FROM_FRAME
.endm

/* EL0-origin prologue/epilogue */
.macro STLX_A64_EL0_ENTRY handler
    STLX_A64_SAVE_GPRS

    /* trap_frame.sp = interrupted user SP (SP_EL0) */
    /* Use x16 (IP0, caller-saved scratch) as temporary. x16 already saved above. */
    mrs x16, sp_el0
    str x16, [sp, #TF_SP]
    add x16, sp, #TF_SIZE
    str x16, [sp, #TF_SP_EL1]

    STLX_A64_CAPTURE_SYSREGS
    STLX_A64_CALL_C \handler
    STLX_A64_RETURN_FROM_FRAME
.endm

/*
 * EL1 with SP0 prologue/epilogue (for elevated tasks).
 *
 * ARCHITECTURAL CONTEXT:
 * When an elevated task (EL1t, SPSEL=0) takes an exception:
 * - Hardware automatically switches to SP_EL1 (system stack)
 * - Hardware saves SPSEL=0 in SPSR_EL1
 * - The task's stack was SP_EL0, NOT the current SP
 *
 * This macro handles elevated tasks exactly like EL0 tasks:
 * - Save SP_EL0 (task's interrupted stack) to trap_frame.sp
 * - Handler runs on SP_EL1 (system stack)
 * - Restore SP_EL0 from trap_frame.sp before eret
 * - eret restores SPSEL=0 from SPSR, automatically using SP_EL0
 *
 * CRITICAL: This is nearly identical to STLX_A64_EL0_ENTRY because
 * both EL0 and elevated EL1t tasks use SP_EL0 as their stack!
 */
.macro STLX_A64_EL1_SP0_ENTRY handler
    STLX_A64_SAVE_GPRS

    /* trap_frame.sp = interrupted task SP (SP_EL0, NOT SP_EL1!) */
    /* Use x16 (IP0, caller-saved scratch) as temporary. x16 already saved above. */
    mrs x16, sp_el0
    str x16, [sp, #TF_SP]
    add x16, sp, #TF_SIZE
    str x16, [sp, #TF_SP_EL1]

    STLX_A64_CAPTURE_SYSREGS
    STLX_A64_CALL_C \handler
    STLX_A64_RETURN_FROM_FRAME
.endm

/* 2KB-aligned vector table */
.align 11
.globl stlx_aarch64_vectors
.type stlx_aarch64_vectors, %function
stlx_aarch64_vectors:
    /* Current EL with SP0 */
    .align 7
    b stlx_aarch64_el1_sp0_sync
    .align 7
    b stlx_aarch64_el1_sp0_irq
    .align 7
    b stlx_aarch64_el1_sp0_fiq
    .align 7
    b stlx_aarch64_el1_sp0_serror

    /* Current EL with SPx */
    .align 7
    b stlx_aarch64_el1_spx_sync
    .align 7
    b stlx_aarch64_el1_spx_irq
    .align 7
    b stlx_aarch64_el1_spx_fiq
    .align 7
    b stlx_aarch64_el1_spx_serror

    /* Lower EL (AArch64) */
    .align 7
    b stlx_aarch64_el0_sync
    .align 7
    b stlx_aarch64_el0_irq
    .align 7
    b stlx_aarch64_el0_fiq
    .align 7
    b stlx_aarch64_el0_serror

    /* Lower EL (AArch32) */
    .align 7
    b stlx_aarch64_el0_a32_sync
    .align 7
    b stlx_aarch64_el0_a32_irq
    .align 7
    b stlx_aarch64_el0_a32_fiq
    .align 7
    b stlx_aarch64_el0_a32_serror

.size stlx_aarch64_vectors, . - stlx_aarch64_vectors

/* Out-of-line vector handlers */

/* EL1 with SP0 (elevated tasks) - use SP0 entry macro to save/restore SP_EL0 */
stlx_aarch64_el1_sp0_sync:
    STLX_A64_EL1_SP0_ENTRY stlx_aarch64_el1_sync_handler
stlx_aarch64_el1_sp0_irq:
    STLX_A64_EL1_SP0_ENTRY stlx_aarch64_el1_irq_handler
stlx_aarch64_el1_sp0_fiq:
    STLX_A64_EL1_SP0_ENTRY stlx_aarch64_el1_fiq_handler
stlx_aarch64_el1_sp0_serror:
    STLX_A64_EL1_SP0_ENTRY stlx_aarch64_el1_serror_handler

/* EL1 with SPx (normal kernel) - use standard EL1 entry macro */
stlx_aarch64_el1_spx_sync:
    STLX_A64_EL1_ENTRY stlx_aarch64_el1_sync_handler
stlx_aarch64_el1_spx_irq:
    STLX_A64_EL1_ENTRY stlx_aarch64_el1_irq_handler
stlx_aarch64_el1_spx_fiq:
    STLX_A64_EL1_ENTRY stlx_aarch64_el1_fiq_handler
stlx_aarch64_el1_spx_serror:
    STLX_A64_EL1_ENTRY stlx_aarch64_el1_serror_handler

stlx_aarch64_el0_sync:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_sync_handler
stlx_aarch64_el0_irq:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_irq_handler
stlx_aarch64_el0_fiq:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_fiq_handler
stlx_aarch64_el0_serror:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_serror_handler

stlx_aarch64_el0_a32_sync:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_sync_handler
stlx_aarch64_el0_a32_irq:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_irq_handler
stlx_aarch64_el0_a32_fiq:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_fiq_handler
stlx_aarch64_el0_a32_serror:
    STLX_A64_EL0_ENTRY stlx_aarch64_el0_serror_handler

