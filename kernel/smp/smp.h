#ifndef STELLUX_SMP_SMP_H
#define STELLUX_SMP_SMP_H

#include "common/types.h"
#include "sync/atomic.h"

namespace smp {

// A wake request cannot be recalled, so a CPU that misses its boot deadline
// may still start later. CLAIMED and ABANDONED are the two outcomes of the
// race between the AP taking ownership of its boot memory and the BSP giving
// up on it, decided by a single compare-exchange so exactly one side wins.
constexpr uint32_t CPU_OFFLINE   = 0;
constexpr uint32_t CPU_BOOTING   = 1; // wake sent, AP has not responded yet
constexpr uint32_t CPU_ONLINE    = 2;
constexpr uint32_t CPU_CLAIMED   = 3; // AP owns its boot memory, still initializing
constexpr uint32_t CPU_ABANDONED = 4; // BSP gave up first, AP must not proceed

struct cpu_info {
    uint32_t logical_id;            // 0-based index
    uint64_t hw_id;                 // APIC ID (x86) or MPIDR (aarch64)
    sync::atomic<uint32_t> state;   // CPU_* above
    bool     is_bsp;                // true for the bootstrap processor
};

constexpr int32_t OK               = 0;
constexpr int32_t ERR_NO_CPUS      = -1;
constexpr int32_t ERR_BOOT_TIMEOUT = -2;
constexpr int32_t ERR_PREPARE      = -3;

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
