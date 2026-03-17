#include "arch/arch_msi.h"
#include "defs/vectors.h"
#include "irq/irq_arch.h"
#include "smp/smp.h"
#include "hw/mmio.h"
#include "common/logging.h"

namespace arch {

static constexpr uint32_t MSI_ADDR_BASE = 0xFEE00000;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_init(uint32_t* out_capacity) {
    *out_capacity = x86::VEC_MSI_COUNT;
    log::info("msi: x86_64 LAPIC backend, %u vectors (IDT 0x%02x-0x%02x)",
              static_cast<uint32_t>(x86::VEC_MSI_COUNT),
              static_cast<uint32_t>(x86::VEC_MSI_BASE),
              static_cast<uint32_t>(x86::VEC_MSI_BASE + x86::VEC_MSI_COUNT - 1));
    return msi::OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t msi_compose(uint32_t vector, uint32_t target_cpu,
                                      msi::message* out) {
    if (vector >= x86::VEC_MSI_COUNT || out == nullptr) {
        return msi::ERR_INVALID;
    }

    uint8_t apic_id = 0;
    smp::cpu_info* info = smp::get_cpu_info(target_cpu);
    if (info) {
        apic_id = static_cast<uint8_t>(info->hw_id);
    } else if (target_cpu == 0) {
        apic_id = static_cast<uint8_t>(
            mmio::read32(irq::get_lapic_va() + irq::LAPIC_ID) >> 24);
    } else {
        return msi::ERR_INVALID;
    }

    out->address = MSI_ADDR_BASE | (static_cast<uint64_t>(apic_id) << 12);
    out->data = x86::VEC_MSI_BASE + vector;
    return msi::OK;
}

} // namespace arch
