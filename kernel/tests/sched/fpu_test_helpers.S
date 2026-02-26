/*
 * FPU test helpers for context isolation verification.
 * Provides functions to write/read a 64-bit fingerprint to/from V0.
 */

#ifdef __aarch64__

.section .text, "ax", @progbits

/* void fpu_test_write_v0(uint64_t value) -- writes value to lower 64 bits of V0 */
.global fpu_test_write_v0
.type fpu_test_write_v0, %function
fpu_test_write_v0:
    fmov d0, x0
    ret
.size fpu_test_write_v0, . - fpu_test_write_v0

/* uint64_t fpu_test_read_v0() -- reads lower 64 bits of V0 */
.global fpu_test_read_v0
.type fpu_test_read_v0, %function
fpu_test_read_v0:
    fmov x0, d0
    ret
.size fpu_test_read_v0, . - fpu_test_read_v0

#endif /* __aarch64__ */

#ifdef __x86_64__

.section .text, "ax", @progbits

/* void fpu_test_write_xmm0(uint64_t value) -- writes value to low 64 bits of XMM0 */
.global fpu_test_write_xmm0
.type fpu_test_write_xmm0, @function
fpu_test_write_xmm0:
    movq %rdi, %xmm0
    retq
.size fpu_test_write_xmm0, . - fpu_test_write_xmm0

/* uint64_t fpu_test_read_xmm0() -- reads low 64 bits of XMM0 */
.global fpu_test_read_xmm0
.type fpu_test_read_xmm0, @function
fpu_test_read_xmm0:
    movq %xmm0, %rax
    retq
.size fpu_test_read_xmm0, . - fpu_test_read_xmm0

#endif /* __x86_64__ */
