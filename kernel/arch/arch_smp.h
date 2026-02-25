#ifndef STELLUX_ARCH_SMP_H
#define STELLUX_ARCH_SMP_H

#include "common/types.h"
#include "smp/smp.h"

namespace arch {

/**
 * @brief Enumerate CPUs from the parsed ACPI MADT.
 * Fills the cpu_info array with one entry per CPU, marking which is the BSP.
 * @param cpus Output array to populate.
 * @param max Maximum entries (array capacity).
 * @return Number of CPUs found (including BSP).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t smp_enumerate(smp::cpu_info* cpus, uint32_t max);

} // namespace arch

#endif // STELLUX_ARCH_SMP_H
