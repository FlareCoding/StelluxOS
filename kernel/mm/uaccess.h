#ifndef STELLUX_MM_UACCESS_H
#define STELLUX_MM_UACCESS_H

#include "common/types.h"

namespace mm::uaccess {

constexpr int32_t OK           = 0;
constexpr int32_t ERR_INVAL    = -1;
constexpr int32_t ERR_NO_MMCTX = -2;
constexpr int32_t ERR_FAULT    = -3;
constexpr int32_t ERR_NAMETOOLONG = -4;

/**
 * @brief Validate that a user range is mapped and has required protections.
 * @param user_ptr User virtual address.
 * @param len Range length in bytes.
 * @param required_prot mm::MM_PROT_READ and/or mm::MM_PROT_WRITE.
 * @return OK on success, negative error on invalid/unmapped/protection fault.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t validate_user_range(
    const void* user_ptr,
    size_t len,
    uint32_t required_prot
);

/**
 * @brief Copy from user buffer to kernel buffer after validation.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_from_user(
    void* kdst,
    const void* usrc,
    size_t len
);

/**
 * @brief Copy from kernel buffer to user buffer after validation.
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

} // namespace mm::uaccess

#endif // STELLUX_MM_UACCESS_H
