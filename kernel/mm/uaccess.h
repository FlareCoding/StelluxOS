#ifndef STELLUX_MM_UACCESS_H
#define STELLUX_MM_UACCESS_H

#include "common/types.h"

namespace mm {
struct mm_context;
}

namespace mm::uaccess {

constexpr int32_t OK           = 0;
constexpr int32_t ERR_INVAL    = -1;
constexpr int32_t ERR_NO_MMCTX = -2;
constexpr int32_t ERR_FAULT    = -3;
constexpr int32_t ERR_NAMETOOLONG = -4;
constexpr int32_t ERR_RETRY    = -5;

/**
 * @brief Copy from a user buffer to a kernel buffer. A user page that is
 * missing or unreadable at the moment of the copy reports ERR_FAULT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_from_user(
    void* kdst,
    const void* usrc,
    size_t len
);

/**
 * @brief Load one 32-bit aligned word from user memory in a single access,
 * so a concurrent store by another thread is never observed torn.
 * @return OK, ERR_INVAL for a misaligned address, ERR_FAULT when the page is
 * missing or unreadable at the moment of the load.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t load_u32_from_user(
    const uint32_t* usrc,
    uint32_t* out
);

/**
 * @brief Atomically store `desired` over the 32-bit aligned user word at
 * `udst` if it holds `*expected`, fully ordered when it stores.
 * @param expected The value the word must hold, replaced by the value found.
 * @return OK when `desired` was stored. ERR_RETRY when it was not, because the
 * word held another value or because the exclusive access was lost, which
 * some architectures allow even when the word matched. ERR_INVAL for a
 * misaligned address, ERR_FAULT when the access faults, as a missing page
 * always does with interrupts masked.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t cmpxchg_u32_in_user(
    uint32_t* udst,
    uint32_t* expected,
    uint32_t desired
);

/**
 * @brief Copy from a kernel buffer to a user buffer. A user page that is
 * missing or read-only at the moment of the copy reports ERR_FAULT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_to_user(
    void* udst,
    const void* ksrc,
    size_t len
);

/**
 * @brief Copy a NUL-terminated string from user buffer with cap.
 * @param kdst Destination kernel buffer.
 * @param cap Destination capacity including terminator.
 * @param usrc User string pointer.
 * @return OK on success, ERR_NAMETOOLONG if no NUL within cap.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_cstr_from_user(
    char* kdst,
    size_t cap,
    const char* usrc
);

/**
 * @brief Copy to a user range with interrupts masked, where a fault on a
 * lazy user page cannot be served. Missing pages are faulted in under the
 * address-space lock first, so the copy itself can never fault.
 * @param can_sleep Whether the lock may be waited for. Interrupt context
 * passes false, so a contended lock reports ERR_RETRY instead.
 * @return OK on success, ERR_RETRY when the lock is contended and the caller
 * cannot sleep, ERR_FAULT/ERR_INVAL on a bad range.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_to_user_irqs_masked(
    void* udst,
    const void* ksrc,
    size_t len,
    bool can_sleep
);

/**
 * @brief Resolve a kernel-mode page fault raised by a user copy. A missing
 * page is faulted in when the context may sleep, otherwise the copy is
 * resumed at its fixup, where it reports the failure to its caller.
 * @param mm_ctx Address space of the faulting task.
 * @param pc Faulting program counter, moved to the fixup on failure.
 * @param fault_addr Faulting virtual address.
 * @param pf_flags mm::PF_FLAG_* describing the access.
 * @param can_sleep Whether interrupts were enabled at the fault.
 * @return true if execution can resume at *pc, false if the fault was not
 * raised by a user copy and must be treated as fatal.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool handle_kernel_fault(
    mm_context* mm_ctx,
    uintptr_t* pc,
    uintptr_t fault_addr,
    uint64_t pf_flags,
    bool can_sleep
);

} // namespace mm::uaccess

#endif // STELLUX_MM_UACCESS_H
