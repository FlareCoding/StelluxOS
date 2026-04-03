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

/**
 * @brief Load the trap table on the current CPU without rebuilding it.
 * x86_64: loads IDTR pointing to the global IDT (built by init()).
 * AArch64: writes VBAR_EL1 (already set by trampoline, so this is a no-op stub).
 * Used by APs after the BSP has called init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void load();

} // namespace trap

#endif // STELLUX_TRAP_TRAP_H
