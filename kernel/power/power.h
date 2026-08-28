#ifndef STELLUX_POWER_POWER_H
#define STELLUX_POWER_POWER_H

#include "common/types.h"

namespace power {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_UNSUPPORTED = -1;

/**
 * @brief Power off the machine.
 * Tries every mechanism the platform offers, so it returns only when
 * all of them failed or none is available. The caller decides whether
 * to halt or report the error.
 * @return ERR_UNSUPPORTED if the platform cannot power off.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t shutdown();

/**
 * @brief Reboot the machine.
 * Tries every mechanism the platform offers, so it returns only when
 * all of them failed or none is available. The caller decides whether
 * to halt or report the error.
 * @return ERR_UNSUPPORTED if the platform cannot reboot.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t reboot();

} // namespace power

#endif // STELLUX_POWER_POWER_H
