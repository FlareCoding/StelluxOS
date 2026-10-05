#include "irq/lapic.h"
#include "irq/irq.h"
#include "acpi/madt_arch.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "common/logging.h"

namespace irq {

constexpr uint32_t ICR_DELIVERY_BUSY = (1 << 12); // Set until the target accepts the last IPI
constexpr uint32_t ICR_DEST_SHIFT    = 24;        // Position of the destination APIC ID in ICR_HIGH
constexpr uint32_t LAPIC_ID_SHIFT    = 24;        // Position of the APIC ID in LAPIC_ID

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
__PRIVILEGED_CODE uint32_t read_lapic_id() {
    return read_lapic_register(LAPIC_ID) >> LAPIC_ID_SHIFT;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wait_icr_idle() {
    while (read_lapic_register(LAPIC_ICR_LOW) & ICR_DELIVERY_BUSY) {
        cpu::relax();
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void send_lapic_ipi(uint32_t apic_id, uint32_t command) {
    // The destination and command halves must land as a pair, an interrupt
    // handler sending its own IPI between them would redirect this one.
    uint64_t flags = cpu::irq_save();
    wait_icr_idle();

    write_lapic_register(LAPIC_ICR_HIGH, apic_id << ICR_DEST_SHIFT);
    write_lapic_register(LAPIC_ICR_LOW, command);

    cpu::irq_restore(flags);
}

} // namespace irq
