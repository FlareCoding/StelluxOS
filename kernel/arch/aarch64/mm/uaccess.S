/*
 * uaccess.S - fault-tolerant user memory copy for AArch64
 *
 * Every load and store sits between the begin and end labels so the data
 * abort handler can recognize a fault raised by the copy and resume at the
 * fixup, where x2 still counts the bytes not yet stored.
 */

.section .priv.text, "ax", @progbits

/*
 * size_t stlx_user_copy(void* dst, const void* src, size_t len)
 *
 * Returns the number of bytes left uncopied, zero on success.
 */
.global stlx_user_copy
.type stlx_user_copy, %function

stlx_user_copy:
.global stlx_user_copy_begin

stlx_user_copy_begin:
    orr x3, x0, x1
    tst x3, #7
    b.ne 2f
1:  cmp x2, #8
    b.lo 2f
    ldr x3, [x1], #8
    str x3, [x0], #8
    sub x2, x2, #8
    b 1b
2:  cbz x2, 3f
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    sub x2, x2, #1
    b 2b
3:  mov x0, #0
    ret

.global stlx_user_copy_end
stlx_user_copy_end:

.global stlx_user_copy_fixup
stlx_user_copy_fixup:
    mov x0, x2
    ret

.size stlx_user_copy, . - stlx_user_copy

/*
 * size_t stlx_user_load32(const uint32_t* src, uint32_t* out)
 *
 * Loads one aligned word in a single access, returns zero on success and
 * one when the source faulted.
 */
.global stlx_user_load32
.type stlx_user_load32, %function

stlx_user_load32:
.global stlx_user_load32_begin

stlx_user_load32_begin:
    ldr w2, [x0]

.global stlx_user_load32_end
stlx_user_load32_end:
    str w2, [x1]
    mov x0, #0
    ret

.global stlx_user_load32_fixup
stlx_user_load32_fixup:
    mov x0, #1
    ret

.size stlx_user_load32, . - stlx_user_load32
