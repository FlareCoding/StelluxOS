#include "irq/lapic.h"
#include "irq/irq.h"
#include "acpi/madt_arch.h"
#include "hw/mmio.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "common/logging.h"

namespace irq {

__PRIVILEGED_BSS static uintptr_t g_lapic_va;
__PRIVILEGED_BSS static uintptr_t g_lapic_base_kva;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_lapic() {
    const auto& madt = acpi::get_madt_info();
    if (madt.lapic_base == 0) {
        log::error("irq: LAPIC base address is zero");
        return ERR_NO_MADT;
    }

    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(madt.lapic_base),
        pmm::PAGE_SIZE,
        paging::PAGE_KERNEL_RW,
        g_lapic_base_kva,
        g_lapic_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map LAPIC at 0x%lx", madt.lapic_base);
        return ERR_MAP;
    }

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t read_lapic_register(uint32_t offset) {
    return mmio::read32(g_lapic_va + offset);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void write_lapic_register(uint32_t offset, uint32_t value) {
    mmio::write32(g_lapic_va + offset, value);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t get_lapic_va() {
    return g_lapic_va;
}

} // namespace irq
