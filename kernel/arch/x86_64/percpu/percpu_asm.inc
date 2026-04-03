/*
 * x86_64 per-CPU assembly macros.
 */

.extern __percpu_start

/*
 * this_cpu_read - Read a per-CPU variable into a register.
 *
 * @result:   Register to store the result (will be clobbered as scratch)
 * @variable: The per-CPU variable symbol to read
 *
 * Note: This clobbers the result register during computation.
 */
.macro this_cpu_read result:req, variable:req
    lea \result, \variable
    sub \result, offset __percpu_start
    mov \result, qword ptr gs:[\result]
.endm
