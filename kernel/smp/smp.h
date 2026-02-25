#ifndef STELLUX_SMP_SMP_H
#define STELLUX_SMP_SMP_H

#include "common/types.h"

namespace smp {

constexpr uint32_t CPU_OFFLINE = 0;
constexpr uint32_t CPU_BOOTING = 1;
constexpr uint32_t CPU_ONLINE  = 2;

struct cpu_info {
    uint32_t logical_id; // 0-based index
    uint64_t hw_id;      // APIC ID (x86) or MPIDR (aarch64)
    uint32_t state;      // CPU_OFFLINE / CPU_BOOTING / CPU_ONLINE
    bool     is_bsp;     // true for the bootstrap processor
};

constexpr int32_t OK         = 0;
constexpr int32_t ERR_NO_CPUS = -1;

/**
 * @brief Enumerate CPUs from ACPI MADT and initialize the SMP subsystem.
 * Marks the BSP as online, all APs as offline. Call after acpi::init().
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Number of CPUs enumerated (including BSP).
 */
uint32_t cpu_count();

/**
 * @brief Number of CPUs currently in CPU_ONLINE state.
 */
uint32_t online_count();

/**
 * @brief Get CPU info by logical ID. Returns nullptr if out of range.
 */
cpu_info* get_cpu_info(uint32_t logical_id);

} // namespace smp

#endif // STELLUX_SMP_SMP_H
