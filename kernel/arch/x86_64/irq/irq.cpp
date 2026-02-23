#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "acpi/madt_arch.h"
#include "defs/vectors.h"
#include "hw/portio.h"
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
__PRIVILEGED_CODE uintptr_t get_lapic_va() {
    return g_lapic_va;
}

/**
 * Mask both 8259 PICs to prevent spurious legacy interrupts.
 * Harmless if the PIC is not present.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void mask_legacy_pic() {
    portio::out8(0x21, 0xFF); // master PIC data
    portio::out8(0xA1, 0xFF); // slave PIC data
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    mask_legacy_pic();

    const auto& madt = acpi::get_madt_info();
    if (madt.lapic_base == 0) {
        log::error("irq: LAPIC base address is zero");
        return ERR_NO_MADT;
    }

    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(madt.lapic_base),
        4096,
        paging::PAGE_KERNEL_RW,
        g_lapic_base_kva,
        g_lapic_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map LAPIC at 0x%lx", madt.lapic_base);
        return ERR_MAP;
    }

    // Mask all LVT entries to clear any stale vectors left by UEFI firmware
    mmio::write32(g_lapic_va + LAPIC_LVT_TIMER,   LVT_MASKED);
    mmio::write32(g_lapic_va + LAPIC_LVT_THERMAL,  LVT_MASKED);
    mmio::write32(g_lapic_va + LAPIC_LVT_PERFCNT,  LVT_MASKED);
    mmio::write32(g_lapic_va + LAPIC_LVT_LINT0,    LVT_MASKED);
    mmio::write32(g_lapic_va + LAPIC_LVT_LINT1,    LVT_MASKED);
    mmio::write32(g_lapic_va + LAPIC_LVT_ERROR,    LVT_MASKED);

    // Enable LAPIC: preserve reserved SVR bits, set enable + spurious vector
    uint32_t svr = mmio::read32(g_lapic_va + LAPIC_SVR);
    svr = (svr & ~static_cast<uint32_t>(0xFF)) | 0x1FF;
    mmio::write32(g_lapic_va + LAPIC_SVR, svr);

    // Clear any stale interrupt state
    mmio::write32(g_lapic_va + LAPIC_EOI, 0);

    log::info("irq: LAPIC enabled at 0x%lx (spurious=0x%02x)",
              madt.lapic_base,
              static_cast<uint32_t>(x86::VEC_SPURIOUS));

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void eoi(uint32_t) {
    mmio::write32(g_lapic_va + LAPIC_EOI, 0);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmask(uint32_t irq) {
    if (irq == x86::VEC_TIMER) {
        uint32_t lvt = mmio::read32(g_lapic_va + LAPIC_LVT_TIMER);
        lvt &= ~LVT_MASKED;
        mmio::write32(g_lapic_va + LAPIC_LVT_TIMER, lvt);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mask(uint32_t irq) {
    if (irq == x86::VEC_TIMER) {
        uint32_t lvt = mmio::read32(g_lapic_va + LAPIC_LVT_TIMER);
        lvt |= LVT_MASKED;
        mmio::write32(g_lapic_va + LAPIC_LVT_TIMER, lvt);
    }
}

} // namespace irq
