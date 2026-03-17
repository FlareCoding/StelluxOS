#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "acpi/madt_arch.h"
#include "hw/mmio.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "common/logging.h"

namespace irq {

__PRIVILEGED_BSS static uintptr_t g_gicd_va;
__PRIVILEGED_BSS static uintptr_t g_gicd_base_kva;
__PRIVILEGED_BSS static uintptr_t g_gicc_va;
__PRIVILEGED_BSS static uintptr_t g_gicc_base_kva;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t acknowledge() {
    return mmio::read32(g_gicc_va + GICC_IAR) & GIC_INTID_MASK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    const auto& madt = acpi::get_madt_info();

    if (madt.gicd_base == 0 || madt.cpu_count == 0) {
        log::error("irq: no GIC info in MADT");
        return ERR_NO_MADT;
    }

    uint64_t gicc_phys = madt.giccs[0].base_address;

    // Map GICD (64KB)
    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(madt.gicd_base),
        0x10000,
        paging::PAGE_KERNEL_RW,
        g_gicd_base_kva,
        g_gicd_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map GICD at 0x%lx", madt.gicd_base);
        return ERR_MAP;
    }

    // Map GICC (64KB)
    rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(gicc_phys),
        0x10000,
        paging::PAGE_KERNEL_RW,
        g_gicc_base_kva,
        g_gicc_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map GICC at 0x%lx", gicc_phys);
        return ERR_MAP;
    }

    // Disable distributor while configuring
    mmio::write32(g_gicd_va + GICD_CTLR, 0);

    // Set priority mask to accept all priorities
    mmio::write32(g_gicc_va + GICC_PMR, 0xFF);

    // Enable CPU interface
    mmio::write32(g_gicc_va + GICC_CTLR, 0x1);

    // Enable distributor (groups 0 and 1)
    mmio::write32(g_gicd_va + GICD_CTLR, 0x3);

    log::info("irq: GICv%u initialized (GICD=0x%lx GICC=0x%lx)",
              static_cast<uint32_t>(madt.gic_version),
              madt.gicd_base,
              gicc_phys);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void eoi(uint32_t irq) {
    mmio::write32(g_gicc_va + GICC_EOIR, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_spi_target(uint32_t irq, uint8_t cpu_mask) {
    uint32_t reg_offset = (irq / 4) * 4;
    uint32_t byte_shift = (irq % 4) * 8;
    uintptr_t addr = g_gicd_va + GICD_ITARGETSR + reg_offset;
    uint32_t val = mmio::read32(addr);
    val &= ~(0xFFu << byte_shift);
    val |= (static_cast<uint32_t>(cpu_mask) << byte_shift);
    mmio::write32(addr, val);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmask(uint32_t irq) {
    uint32_t bank = irq / 32;
    uint32_t bit = irq % 32;
    mmio::write32(g_gicd_va + GICD_ISENABLER + bank * 4, 1u << bit);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mask(uint32_t irq) {
    uint32_t bank = irq / 32;
    uint32_t bit = irq % 32;
    mmio::write32(g_gicd_va + GICD_ICENABLER + bank * 4, 1u << bit);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_group1(uint32_t irq) {
    uint32_t bank = irq / 32;
    uint32_t bit = irq % 32;
    uintptr_t addr = g_gicd_va + GICD_IGROUPR + bank * 4;
    uint32_t val = mmio::read32(addr);
    val |= (1u << bit);
    mmio::write32(addr, val);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_level_triggered(uint32_t irq) {
    uint32_t reg_index = irq / 16;
    uint32_t field_shift = (irq % 16) * 2;
    uintptr_t addr = g_gicd_va + GICD_ICFGR + reg_index * 4;
    uint32_t val = mmio::read32(addr);
    val &= ~(0x3u << field_shift);
    mmio::write32(addr, val);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_edge_triggered(uint32_t irq) {
    uint32_t reg_index = irq / 16;
    uint32_t field_shift = (irq % 16) * 2;
    uintptr_t addr = g_gicd_va + GICD_ICFGR + reg_index * 4;
    uint32_t val = mmio::read32(addr);
    val &= ~(0x3u << field_shift);
    val |= (0x2u << field_shift);
    mmio::write32(addr, val);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap() {
    mmio::write32(g_gicc_va + GICC_PMR, 0xFF);
    mmio::write32(g_gicc_va + GICC_CTLR, 0x1);
    return OK;
}

} // namespace irq
