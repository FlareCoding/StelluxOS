#include "irq/gic.h"
#include "irq/irq.h"
#include "acpi/madt_arch.h"
#include "hw/mmio.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "percpu/percpu.h"
#include "common/logging.h"

namespace irq {

// Distributor registers specific to GICv2
constexpr uint32_t GICD_ITARGETSR = 0x0800;
constexpr uint32_t GICD_SGIR      = 0x0F00;

// CPU interface registers
constexpr uint32_t GICC_CTLR = 0x0000;
constexpr uint32_t GICC_PMR  = 0x0004;
constexpr uint32_t GICC_IAR  = 0x000C;
constexpr uint32_t GICC_EOIR = 0x0010;

constexpr size_t   GIC_FRAME_SIZE          = 0x10000;
constexpr uint32_t GICD_CTLR_ENABLE_GROUPS = 0x3; // Forward group 0 and group 1
constexpr uint32_t GICD_TYPER_CPUS_SHIFT   = 5;   // Implemented CPU interfaces, minus one
constexpr uint32_t GICD_TYPER_CPUS_MASK    = 0x7;
constexpr uint32_t GICD_SGIR_TARGETS_SHIFT = 16;
constexpr uint32_t GICD_SGIR_INTID_MASK    = 0xF;
constexpr uint32_t GICC_CTLR_ENABLE        = 0x1;
constexpr uint32_t GICC_PMR_ALLOW_ALL      = 0xFF;

// A GIC with one CPU interface reads its target registers as zero and
// delivers every interrupt to that interface
constexpr uint8_t UNIPROCESSOR_TARGETS = 0x1;

__PRIVILEGED_BSS static uintptr_t g_gicd_va;
__PRIVILEGED_BSS static uintptr_t g_gicd_base_kva;
__PRIVILEGED_BSS static uintptr_t g_gicc_va;
__PRIVILEGED_BSS static uintptr_t g_gicc_base_kva;
__PRIVILEGED_BSS static uint32_t  g_intid_end;               // One past the highest implemented interrupt ID
__PRIVILEGED_BSS static uint8_t   g_interface_bit[MAX_CPUS]; // Each CPU's target list bit, 0 until it initializes

/**
 * Reads the calling CPU's target list bit. The target fields of SGIs and PPIs
 * are banked and name only the reading CPU, or read as zero if unimplemented.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint8_t read_interface_bit() {
    uint32_t typer = mmio::read32(g_gicd_va + GICD_TYPER);
    if (((typer >> GICD_TYPER_CPUS_SHIFT) & GICD_TYPER_CPUS_MASK) == 0) {
        return UNIPROCESSOR_TARGETS;
    }

    for (uint32_t intid = 0; intid < GIC_SPI_BASE; intid++) {
        uint8_t targets = mmio::read8(g_gicd_va + GICD_ITARGETSR + intid);
        if (targets != 0) {
            return targets;
        }
    }

    return 0;
}

/**
 * Records how the distributor addresses the calling CPU, then enables the
 * CPU's interface.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t init_this_cpu() {
    uint32_t cpu = percpu::current_cpu_id();
    uint8_t targets = read_interface_bit();
    if (targets == 0) {
        log::error("irq: GICv2 reports no CPU interface for CPU %u", cpu);
        return ERR_NO_CPU_INTERFACE;
    }

    g_interface_bit[cpu] = targets;
    mmio::write32(g_gicc_va + GICC_PMR, GICC_PMR_ALLOW_ALL);
    mmio::write32(g_gicc_va + GICC_CTLR, GICC_CTLR_ENABLE);
    return OK;
}

/**
 * @brief The target list bit of logical CPU `cpu`, or 0 while it cannot be targeted.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint8_t interface_bit_of(uint32_t cpu) {
    return cpu < MAX_CPUS ? g_interface_bit[cpu] : 0;
}

/**
 * Sets the CPUs an SPI is delivered to. Each SPI has its own byte, so CPUs
 * retargeting neighboring SPIs never undo each other's writes.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void write_targets(uint32_t intid, uint8_t targets) {
    mmio::write8(g_gicd_va + GICD_ITARGETSR + intid, targets);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv2_init(const acpi::madt_info& madt) {
    uint64_t gicc_phys = madt.giccs[0].base_address;

    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(madt.gicd_base),
        GIC_FRAME_SIZE,
        paging::PAGE_KERNEL_RW,
        g_gicd_base_kva,
        g_gicd_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map GICD at 0x%lx", madt.gicd_base);
        return ERR_MAP;
    }

    rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(gicc_phys),
        GIC_FRAME_SIZE,
        paging::PAGE_KERNEL_RW,
        g_gicc_base_kva,
        g_gicc_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map GICC at 0x%lx", gicc_phys);
        return ERR_MAP;
    }

    uint32_t typer = mmio::read32(g_gicd_va + GICD_TYPER);
    g_intid_end = GIC_INTIDS_PER_REG * ((typer & GICD_TYPER_LINES) + 1);
    if (g_intid_end > GIC_SPI_END) {
        g_intid_end = GIC_SPI_END;
    }

    // The distributor stays off until every SPI has a target and this CPU can take interrupts
    mmio::write32(g_gicd_va + GICD_CTLR, 0);
    rc = init_this_cpu();
    if (rc != OK) {
        return rc;
    }

    uint8_t boot_targets = g_interface_bit[percpu::current_cpu_id()];
    for (uint32_t intid = GIC_SPI_BASE; intid < g_intid_end; intid++) {
        write_targets(intid, boot_targets);
    }
    mmio::write32(g_gicd_va + GICD_CTLR, GICD_CTLR_ENABLE_GROUPS);

    log::info("irq: GICv2 initialized (GICD=0x%lx GICC=0x%lx, %u interrupt IDs)",
              madt.gicd_base, gicc_phys, g_intid_end);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv2_init_ap() {
    return init_this_cpu();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t gicv2_acknowledge() {
    return mmio::read32(g_gicc_va + GICC_IAR);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv2_eoi(uint32_t ack) {
    mmio::write32(g_gicc_va + GICC_EOIR, ack);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv2_send_sgi(uint32_t intid, uint32_t target_cpu) {
    uint8_t targets = interface_bit_of(target_cpu);
    if (targets == 0) {
        return ERR_INVAL;
    }

    // A zero target list filter delivers to exactly the listed interfaces
    uint32_t sgir = (static_cast<uint32_t>(targets) << GICD_SGIR_TARGETS_SHIFT) |
                    (intid & GICD_SGIR_INTID_MASK);
    mmio::write32(g_gicd_va + GICD_SGIR, sgir);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv2_set_spi_target(uint32_t intid, uint32_t target_cpu) {
    uint8_t targets = interface_bit_of(target_cpu);
    if (intid < GIC_SPI_BASE || intid >= g_intid_end || targets == 0) {
        return ERR_INVAL;
    }

    write_targets(intid, targets);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv2_unmask(uint32_t intid) {
    mmio::write32(g_gicd_va + GICD_ISENABLER + bit_array_offset(intid), bit_array_mask(intid));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv2_mask(uint32_t intid) {
    mmio::write32(g_gicd_va + GICD_ICENABLER + bit_array_offset(intid), bit_array_mask(intid));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv2_set_group1(uint32_t intid) {
    uintptr_t addr = g_gicd_va + GICD_IGROUPR + bit_array_offset(intid);
    mmio::write32(addr, mmio::read32(addr) | bit_array_mask(intid));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv2_set_trigger(uint32_t intid, trigger mode) {
    uintptr_t addr = g_gicd_va + GICD_ICFGR + icfgr_offset(intid);
    uint32_t shift = icfgr_shift(intid);

    uint32_t val = mmio::read32(addr) & ~(GIC_ICFGR_FIELD_MASK << shift);
    if (mode == trigger::edge) {
        val |= GICD_ICFGR_EDGE << shift;
    }
    mmio::write32(addr, val);
}

static const gic_backend g_gicv2 = {
    .init           = gicv2_init,
    .init_ap        = gicv2_init_ap,
    .acknowledge    = gicv2_acknowledge,
    .eoi            = gicv2_eoi,
    .send_sgi       = gicv2_send_sgi,
    .set_spi_target = gicv2_set_spi_target,
    .unmask         = gicv2_unmask,
    .mask           = gicv2_mask,
    .set_group1     = gicv2_set_group1,
    .set_trigger    = gicv2_set_trigger,
};

const gic_backend& gicv2_backend() {
    return g_gicv2;
}

} // namespace irq
