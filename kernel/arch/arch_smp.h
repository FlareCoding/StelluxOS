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

/**
 * @brief Prepare the interrupt path that `smp_raise_ipi` uses, on the BSP.
 * x86_64: nothing, the IPI vector is routed by the trap entry.
 * AArch64: enables the IPI SGI on this CPU interface.
 * @return smp::ipi::OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_ipi_init();

/**
 * @brief Per-AP counterpart of `smp_ipi_init`, for state banked per CPU.
 * @return smp::ipi::OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_ipi_init_ap();

/**
 * @brief Interrupt `target` on the IPI vector. The caller has already
 * recorded what the target must do, this only delivers the interrupt.
 * x86_64: fixed-delivery IPI through the LAPIC ICR to the target's APIC id.
 * AArch64: software-generated interrupt to the target's CPU interface.
 * @return smp::ipi::OK, or smp::ipi::ERR_UNREACHABLE when the interrupt
 *         controller cannot address the target.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_raise_ipi(const smp::cpu_info& target);

} // namespace arch

#endif // STELLUX_ARCH_SMP_H
