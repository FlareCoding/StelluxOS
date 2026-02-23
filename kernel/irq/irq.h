#ifndef STELLUX_IRQ_IRQ_H
#define STELLUX_IRQ_IRQ_H

#include "common/types.h"

namespace irq {

constexpr int32_t OK          = 0;
constexpr int32_t ERR_NO_MADT = -1;
constexpr int32_t ERR_MAP     = -2;

/**
 * @brief Initialize the interrupt controller hardware.
 * x86_64: masks 8259 PIC, maps and enables LAPIC.
 * AArch64: maps and enables GICv2 distributor + CPU interface.
 * Must be called after acpi::init().
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Signal end-of-interrupt.
 * x86_64: writes LAPIC EOI register (parameter ignored).
 * AArch64: writes interrupt ID to GICC_EOIR.
 * @param irq Interrupt number (used by GIC, ignored by LAPIC).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void eoi(uint32_t irq);

/**
 * @brief Unmask (enable) a specific interrupt line.
 * @param irq Interrupt number / vector to unmask.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmask(uint32_t irq);

/**
 * @brief Mask (disable) a specific interrupt line.
 * @param irq Interrupt number / vector to mask.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mask(uint32_t irq);

} // namespace irq

#endif // STELLUX_IRQ_IRQ_H
