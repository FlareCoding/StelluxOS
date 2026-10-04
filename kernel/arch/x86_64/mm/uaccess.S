/*
 * uaccess.S - fault-tolerant user memory copy for x86_64
 *
 * The copy instruction sits alone between the begin and end labels so the
 * page fault handler can recognize a fault raised by it and resume at the
 * fixup, where rcx still holds the number of bytes not yet copied.
 */

.intel_syntax noprefix

.section .priv.text, "ax", @progbits

/*
 * size_t stlx_user_copy(void* dst, const void* src, size_t len)
 *
 * Returns the number of bytes left uncopied, zero on success.
 */
.global stlx_user_copy
.type stlx_user_copy, @function

stlx_user_copy:
    mov rcx, rdx

.global stlx_user_copy_begin
stlx_user_copy_begin:
    rep movsb

.global stlx_user_copy_end
stlx_user_copy_end:
    xor eax, eax
    ret

.global stlx_user_copy_fixup
stlx_user_copy_fixup:
    mov rax, rcx
    ret

.size stlx_user_copy, . - stlx_user_copy

/*
 * size_t stlx_user_load32(const uint32_t* src, uint32_t* out)
 *
 * Loads one aligned word in a single access, returns zero on success and
 * one when the source faulted.
 */
.global stlx_user_load32
.type stlx_user_load32, @function

stlx_user_load32:

.global stlx_user_load32_begin
stlx_user_load32_begin:
    mov eax, dword ptr [rdi]

.global stlx_user_load32_end
stlx_user_load32_end:
    mov dword ptr [rsi], eax
    xor eax, eax
    ret

.global stlx_user_load32_fixup
stlx_user_load32_fixup:
    mov eax, 1
    ret

.size stlx_user_load32, . - stlx_user_load32

/*
 * size_t stlx_user_cmpxchg32(uint32_t* dst, uint32_t expected, uint32_t desired, uint32_t* observed)
 *
 * Stores desired over the aligned word if it holds expected, in one locked
 * access. Writes the word as found to observed, returns zero when desired
 * was stored, one when it was not, and two when the word faulted.
 */
.global stlx_user_cmpxchg32
.type stlx_user_cmpxchg32, @function

stlx_user_cmpxchg32:
    mov eax, esi

.global stlx_user_cmpxchg32_begin
stlx_user_cmpxchg32_begin:
    lock cmpxchg dword ptr [rdi], edx

.global stlx_user_cmpxchg32_end
stlx_user_cmpxchg32_end:
    mov dword ptr [rcx], eax
    jne .Lcmpxchg32_not_stored
    xor eax, eax
    ret

.Lcmpxchg32_not_stored:
    mov eax, 1
    ret

.global stlx_user_cmpxchg32_fixup
stlx_user_cmpxchg32_fixup:
    mov eax, 2
    ret

.size stlx_user_cmpxchg32, . - stlx_user_cmpxchg32
