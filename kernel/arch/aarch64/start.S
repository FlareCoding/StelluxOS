/*
 * AArch64 Kernel Entry Point
 * 
 * Limine loads us in AArch64 mode with:
 * - MMU enabled (identity + HHDM mapping)
 * - Interrupts disabled (DAIF masked)
 * - A valid stack (but we set our own for safety)
 * - Running at EL1
 */

.section .priv.bss, "aw", @nobits
.align 16
stack_bottom:
    .space 65536   /* 64 KB task stack (SP_EL0) */
.global stack_top
stack_top:

.align 16
sys_stack_bottom:
    .space 16384   /* 16 KB system stack (SP_EL1, used by exception handlers) */
.global sys_stack_top
sys_stack_top:

.section .priv.text, "ax", @progbits
.global _start
.type _start, @function

_start:
    /* 
     * Mask all interrupts (DAIF = Debug, Async, IRQ, FIQ)
     * D=1, A=1, I=1, F=1
     */
    msr daifset, #0xF

    /*
     * Ensure we're using SP_EL1 (not SP_EL0) before initializing the stack.
     * Limine enters at EL1, but SPSel may not be what we expect.
     */
    msr spsel, #1
    isb

    /* Set up our own stack (SP_EL1) */
    adrp x0, stack_top
    add x0, x0, :lo12:stack_top
    mov sp, x0

    /* Clear frame pointer and link register for clean backtraces */
    mov x29, #0
    mov x30, #0

    /* Call the kernel entry point */
    bl stlx_init

    /* If stlx_init returns, halt forever */
.Lhalt:
    wfi
    b .Lhalt

.size _start, . - _start
