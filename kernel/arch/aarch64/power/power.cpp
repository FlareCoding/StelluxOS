#include "power/power.h"
#include "hw/psci.h"
#include "hw/cpu.h"
#include "common/logging.h"

namespace power {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t shutdown() {
    psci::conduit c = psci::detect_conduit();

    // Firmware tears down the whole machine, secondary CPUs included,
    // so no CPU coordination is needed before the call.
    cpu::irq_disable();
    int32_t rc = psci::system_off(c);

    log::warn("power: PSCI SYSTEM_OFF failed (rc=%d)", rc);
    return ERR_UNSUPPORTED;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t reboot() {
    psci::conduit c = psci::detect_conduit();

    cpu::irq_disable();
    int32_t rc = psci::system_reset(c);

    log::warn("power: PSCI SYSTEM_RESET failed (rc=%d)", rc);
    return ERR_UNSUPPORTED;
}

} // namespace power
