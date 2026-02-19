#ifndef STELLUX_MM_MM_H
#define STELLUX_MM_MM_H

#include "common/types.h"

namespace mm {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Initialize the memory management subsystem.
 * Calls PMM, VA layout, KVA, and VMM init in order.
 * Must be called after boot_services::init() and arch::early_init().
 * @return OK on success, ERR on failure (sub-step failure is logged).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace mm

#endif // STELLUX_MM_MM_H
