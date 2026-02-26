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

/**
 * @brief One-time setup before booting any AP.
 * x86_64: identity-maps trampoline region, copies trampoline code, inits startup data.
 * AArch64: allocates trampoline page, builds identity map, detects PSCI conduit.
 * @return smp::OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_prepare();

/**
 * @brief Boot a single AP. Allocates stack, fills startup data, sends wake
 * sequence, polls cpu.state for CPU_ONLINE.
 * x86_64: INIT-SIPI-SIPI via LAPIC ICR.
 * AArch64: PSCI CPU_ON with trampoline entry point.
 * @param cpu The cpu_info entry to boot. State must be CPU_BOOTING on entry.
 * @return smp::OK on success, smp::ERR_BOOT_TIMEOUT if AP did not come online.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_boot_cpu(smp::cpu_info& cpu);

} // namespace arch

#endif // STELLUX_ARCH_SMP_H
