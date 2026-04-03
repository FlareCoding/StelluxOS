#ifndef STELLUX_AARCH64_IRQ_IRQ_ARCH_H
#define STELLUX_AARCH64_IRQ_IRQ_ARCH_H

#include "common/types.h"

namespace irq {

// GICD (Distributor) register offsets
constexpr uint32_t GICD_CTLR       = 0x000;
constexpr uint32_t GICD_TYPER      = 0x004;
constexpr uint32_t GICD_ISENABLER  = 0x100;
constexpr uint32_t GICD_ICENABLER  = 0x180;
constexpr uint32_t GICD_IGROUPR    = 0x080;
constexpr uint32_t GICD_IPRIORITYR = 0x400;
constexpr uint32_t GICD_ITARGETSR  = 0x800;
constexpr uint32_t GICD_ICFGR      = 0xC00;

// GICC (CPU Interface) register offsets
constexpr uint32_t GICC_CTLR       = 0x000;
constexpr uint32_t GICC_PMR        = 0x004;
constexpr uint32_t GICC_IAR        = 0x00C;
constexpr uint32_t GICC_EOIR       = 0x010;

constexpr uint32_t GIC_SPURIOUS_ID = 1023;
constexpr uint32_t GIC_INTID_MASK  = 0x3FF;

/**
 * @brief Read GICC_IAR to acknowledge the current interrupt.
 * Returns the interrupt ID. Must be called from the IRQ trap handler.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t acknowledge();

/**
 * @brief Set target CPU mask for an SPI.
 * SPIs (INTID >= 32) default to no target; must be configured explicitly.
 * @param irq GIC interrupt ID (INTID).
 * @param cpu_mask Bitmask of target CPUs (bit 0 = CPU 0, etc.).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_spi_target(uint32_t irq, uint8_t cpu_mask);

/**
 * @brief Assign an interrupt to Group 1 (non-secure IRQ).
 * Required on platforms where TF-A runs at EL3 and the kernel at EL1
 * can only receive Group 1 interrupts.
 * @param irq GIC interrupt ID (INTID).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_group1(uint32_t irq);

/**
 * @brief Configure an SPI as level-triggered (default GIC reset value
 * may differ on real hardware vs QEMU).
 * @param irq GIC interrupt ID (INTID).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_level_triggered(uint32_t irq);

/**
 * Configure an SPI as edge-triggered. Sets GICD_ICFGR field to 0b10.
 * @param irq GIC interrupt ID (INTID).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_edge_triggered(uint32_t irq);

} // namespace irq

#endif // STELLUX_AARCH64_IRQ_IRQ_ARCH_H
