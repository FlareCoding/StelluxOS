#ifndef STELLUX_ARCH_MSI_H
#define STELLUX_ARCH_MSI_H

#include "common/types.h"
#include "msi/msi.h"

namespace arch {

/**
 * Initialize the platform MSI controller. Report available vector
 * count in *out_capacity (must be <= msi::MAX_VECTORS).
 * @return msi::OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_init(uint32_t* out_capacity);

/**
 * Compose the (address, data) pair for a given vector index targeting
 * a logical CPU. target_cpu is a logical CPU index (0 = BSP);
 * the arch maps it to hardware ID (APIC ID, MPIDR, etc.) internally.
 * @return msi::OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_compose(uint32_t vector,
                                      uint32_t target_cpu,
                                      msi::message* out);

} // namespace arch

#endif // STELLUX_ARCH_MSI_H
