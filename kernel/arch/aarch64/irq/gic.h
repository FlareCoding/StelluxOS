#ifndef STELLUX_ARCH_AARCH64_IRQ_GIC_H
#define STELLUX_ARCH_AARCH64_IRQ_GIC_H

#include "common/types.h"

namespace acpi { struct madt_info; }

namespace irq {

// SPIs use interrupt IDs 32 to 1019. The IDs below them are SGIs and PPIs,
// which are private to each CPU.
constexpr uint32_t GIC_SPI_BASE = 32;
constexpr uint32_t GIC_SPI_END  = 1020;

// Distributor registers that every GIC generation lays out the same way
constexpr uint32_t GICD_CTLR       = 0x0000;
constexpr uint32_t GICD_TYPER      = 0x0004;
constexpr uint32_t GICD_IGROUPR    = 0x0080;
constexpr uint32_t GICD_ISENABLER  = 0x0100;
constexpr uint32_t GICD_ICENABLER  = 0x0180;
constexpr uint32_t GICD_ICPENDR    = 0x0280;
constexpr uint32_t GICD_ICACTIVER  = 0x0380;
constexpr uint32_t GICD_IPRIORITYR = 0x0400;
constexpr uint32_t GICD_ICFGR      = 0x0C00;

constexpr uint32_t GICD_TYPER_LINES = 0x1F; // Implemented interrupt IDs in units of 32, minus one
constexpr uint32_t GICD_ICFGR_EDGE  = 0x2;

// Enable, pending, active, and group registers hold one bit per interrupt,
// GICD_IPRIORITYR one byte, and GICD_ICFGR a two-bit trigger field
constexpr uint32_t GIC_REG_BYTES             = 4;
constexpr uint32_t GIC_INTIDS_PER_REG        = 32;
constexpr uint32_t GIC_INTIDS_PER_IPRIORITYR = 4;
constexpr uint32_t GIC_ICFGR_FIELD_BITS      = 2;
constexpr uint32_t GIC_ICFGR_FIELD_MASK      = 0x3;
constexpr uint32_t GIC_INTIDS_PER_ICFGR      = GIC_INTIDS_PER_REG / GIC_ICFGR_FIELD_BITS;

// How a device signals an interrupt: by holding a level until serviced, or by an edge
enum class trigger : uint8_t {
    level,
    edge,
};

/**
 * A GIC backend drives one generation of the Arm Generic Interrupt Controller.
 * `irq.cpp` selects the backend for the controller the MADT reports and
 * forwards every `irq::` call to it. Callers name CPUs by logical id, and each
 * backend must translate that id into its controller's own addressing. `init`
 * runs on the boot CPU and must route every SPI to it, and `init_ap` runs on
 * each secondary CPU. A CPU can be targeted once it has initialized.
 */
struct gic_backend {
    int32_t  (*init)(const acpi::madt_info& madt);
    int32_t  (*init_ap)();
    uint32_t (*acknowledge)();
    void     (*eoi)(uint32_t ack);
    int32_t  (*send_sgi)(uint32_t intid, uint32_t target_cpu);
    int32_t  (*set_spi_target)(uint32_t intid, uint32_t target_cpu);
    void     (*unmask)(uint32_t intid);
    void     (*mask)(uint32_t intid);
    void     (*set_group1)(uint32_t intid);
    void     (*set_trigger)(uint32_t intid, trigger mode);
};

// Offset of the register holding `intid` in a one bit per interrupt array
constexpr uint32_t bit_array_offset(uint32_t intid) {
    return (intid / GIC_INTIDS_PER_REG) * GIC_REG_BYTES;
}

// Bit of `intid` within that register
constexpr uint32_t bit_array_mask(uint32_t intid) {
    return 1u << (intid % GIC_INTIDS_PER_REG);
}

// Offset of the GICD_ICFGR register holding the trigger field of `intid`
constexpr uint32_t icfgr_offset(uint32_t intid) {
    return (intid / GIC_INTIDS_PER_ICFGR) * GIC_REG_BYTES;
}

// Position of that trigger field within its register
constexpr uint32_t icfgr_shift(uint32_t intid) {
    return (intid % GIC_INTIDS_PER_ICFGR) * GIC_ICFGR_FIELD_BITS;
}

/**
 * @brief GICv2: a memory-mapped distributor and CPU interface, serving at most
 * eight CPUs.
 */
const gic_backend& gicv2_backend();

/**
 * @brief GICv3 and later: an affinity routed distributor, one redistributor
 * per CPU for SGI and PPI state, and a system register CPU interface.
 */
const gic_backend& gicv3_backend();

} // namespace irq

#endif // STELLUX_ARCH_AARCH64_IRQ_GIC_H
