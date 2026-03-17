#include "arch/arch_msi.h"

namespace arch {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_init(uint32_t* out_capacity) {
    *out_capacity = 0;
    return msi::ERR_NOT_SUPPORTED;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_compose(uint32_t, uint32_t, msi::message*) {
    return msi::ERR_NOT_SUPPORTED;
}

} // namespace arch
