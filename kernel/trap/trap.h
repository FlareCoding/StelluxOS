#ifndef STELLUX_TRAP_TRAP_H
#define STELLUX_TRAP_TRAP_H

#include "common/types.h"

namespace trap {

constexpr int32_t OK = 0;
constexpr int32_t ERR_UNSUPPORTED = -1;

/**
 * @brief Installs architecture-specific trap/exception handling (IDT/VBAR).
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace trap

#endif // STELLUX_TRAP_TRAP_H
