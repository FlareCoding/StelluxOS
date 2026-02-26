/*
 * AArch64 FPU/SIMD save/restore routines.
 *
 * Must be in assembly because -mgeneral-regs-only prevents
 * Q register references in Clang inline asm.
 *
 * Layout of sched::fpu_state (528 bytes, 16-byte aligned):
 *   +0x000: V0-V31   (32 x 128-bit = 512 bytes)
 *   +0x200: FPCR     (32-bit)
 *   +0x204: FPSR     (32-bit)
 */

.section .priv.text, "ax", @progbits

/* void fpu_save_state(sched::fpu_state* state) */
.global fpu_save_state
.type fpu_save_state, %function
fpu_save_state:
    stp q0,  q1,  [x0, #0]
    stp q2,  q3,  [x0, #32]
    stp q4,  q5,  [x0, #64]
    stp q6,  q7,  [x0, #96]
    stp q8,  q9,  [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x1, fpcr
    str w1, [x0, #512]
    mrs x1, fpsr
    str w1, [x0, #516]
    ret
.size fpu_save_state, . - fpu_save_state

/* void fpu_restore_state(const sched::fpu_state* state) */
.global fpu_restore_state
.type fpu_restore_state, %function
fpu_restore_state:
    ldp q0,  q1,  [x0, #0]
    ldp q2,  q3,  [x0, #32]
    ldp q4,  q5,  [x0, #64]
    ldp q6,  q7,  [x0, #96]
    ldp q8,  q9,  [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ldr w1, [x0, #512]
    msr fpcr, x1
    ldr w1, [x0, #516]
    msr fpsr, x1
    ret
.size fpu_restore_state, . - fpu_restore_state
