#include "arch/arch_smp.h"
#include "acpi/madt_arch.h"

namespace arch {

static inline uint64_t read_mpidr_el1() {
    uint64_t val;
    asm volatile("mrs %0, mpidr_el1" : "=r"(val));
    return val;
}

constexpr uint64_t MPIDR_AFF_MASK = 0xFF00FFFFFFULL;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t smp_enumerate(smp::cpu_info* cpus, uint32_t max) {
    const acpi::madt_info& madt = acpi::get_madt_info();

    uint64_t current_mpidr = read_mpidr_el1() & MPIDR_AFF_MASK;

    uint32_t count = 0;
    for (uint32_t i = 0; i < madt.cpu_count && count < max; i++) {
        if (!madt.giccs[i].enabled) {
            continue;
        }

        uint64_t entry_mpidr = madt.giccs[i].mpidr & MPIDR_AFF_MASK;

        cpus[count].logical_id = count;
        cpus[count].hw_id = madt.giccs[i].mpidr;
        cpus[count].state = smp::CPU_OFFLINE;
        cpus[count].is_bsp = (entry_mpidr == current_mpidr);
        count++;
    }

    return count;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_prepare() {
    return smp::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t smp_boot_cpu(smp::cpu_info&) {
    return smp::ERR_BOOT_TIMEOUT;
}

} // namespace arch
