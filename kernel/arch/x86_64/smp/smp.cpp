#include "arch/arch_smp.h"
#include "acpi/madt_arch.h"
#include "irq/irq_arch.h"
#include "hw/mmio.h"
#include "hw/msr.h"

namespace arch {

constexpr uint32_t MSR_IA32_APIC_BASE = 0x1B;
constexpr uint64_t APIC_BASE_BSP_FLAG = (1ULL << 8);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t smp_enumerate(smp::cpu_info* cpus, uint32_t max) {
    const acpi::madt_info& madt = acpi::get_madt_info();

    uint8_t bsp_apic_id = 0;
    uint64_t apic_base_msr = msr::read(MSR_IA32_APIC_BASE);
    if (apic_base_msr & APIC_BASE_BSP_FLAG) {
        uintptr_t lapic_va = irq::get_lapic_va();
        bsp_apic_id = static_cast<uint8_t>(mmio::read32(lapic_va + irq::LAPIC_ID) >> 24);
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < madt.lapic_count && count < max; i++) {
        if (!madt.lapics[i].enabled) {
            continue;
        }

        cpus[count].logical_id = count;
        cpus[count].hw_id = madt.lapics[i].apic_id;
        cpus[count].state = smp::CPU_OFFLINE;
        cpus[count].is_bsp = (madt.lapics[i].apic_id == bsp_apic_id);
        count++;
    }

    return count;
}

} // namespace arch
