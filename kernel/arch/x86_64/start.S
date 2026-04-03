/*
 * x86_64 Kernel Entry Point
 * 
 * Limine loads us in 64-bit long mode with:
 * - Paging enabled (identity + HHDM mapping)
 * - GDT set up
 * - Interrupts disabled
 * - A valid stack (but we set our own for safety)
 */

.section .priv.bss, "aw", @nobits
.align 16
stack_bottom:
    .space 65536   /* 64 KB stack */
.global stack_top
stack_top:

.section .priv.text, "ax", @progbits
.intel_syntax noprefix
.global _start
.type _start, @function

_start:
    /* Clear direction flag (required by System V ABI) */
    cld

    /* Set up our own stack */
    lea rsp, [rip + stack_top]

    /* Align stack to 16 bytes (System V ABI requirement) */
    and rsp, ~0xF

    /* Call the kernel entry point */
    call stlx_init

    /* If stlx_init returns, halt forever */
.Lhalt:
    cli
    hlt
    jmp .Lhalt

.size _start, . - _start
