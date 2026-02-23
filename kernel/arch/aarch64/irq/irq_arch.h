#ifndef STELLUX_AARCH64_IRQ_IRQ_ARCH_H
#define STELLUX_AARCH64_IRQ_IRQ_ARCH_H

#include "common/types.h"

namespace irq {

// GICD (Distributor) register offsets
constexpr uint32_t GICD_CTLR       = 0x000;
constexpr uint32_t GICD_TYPER      = 0x004;
constexpr uint32_t GICD_ISENABLER  = 0x100;
constexpr uint32_t GICD_ICENABLER  = 0x180;
constexpr uint32_t GICD_IPRIORITYR = 0x400;

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

} // namespace irq

#endif // STELLUX_AARCH64_IRQ_IRQ_ARCH_H
