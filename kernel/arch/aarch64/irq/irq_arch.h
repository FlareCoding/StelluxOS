#ifndef STELLUX_AARCH64_IRQ_IRQ_ARCH_H
#define STELLUX_AARCH64_IRQ_IRQ_ARCH_H

#include "common/types.h"

namespace irq {

constexpr uint32_t GIC_SPURIOUS_ID = 1023;
constexpr uint32_t GIC_INTID_MASK  = 0x3FF;

// The software-generated interrupt that carries inter-processor messages
constexpr uint32_t IPI_SGI_INTID = 0;

/**
 * @brief Acknowledge the highest priority pending interrupt.
 * Returns the raw acknowledge value: the INTID in the low ten bits and, on
 * GICv2, the source CPU of an SGI above them. The whole value must be passed
 * back to eoi() unchanged. Must be called from the IRQ trap handler.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t acknowledge();

/**
 * @brief Raise SGI `intid` on one CPU.
 * @param intid SGI number, 0 to 15.
 * @param target_cpu Logical id of the target CPU.
 * @return OK, or ERR_INVAL if that CPU has not initialized its GIC interface.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t send_sgi(uint32_t intid, uint32_t target_cpu);

/**
 * @brief Route an SPI to one CPU.
 * The GIC routes every SPI to the boot CPU when it initializes.
 * @param irq GIC interrupt ID (INTID) of an SPI.
 * @param target_cpu Logical id of the target CPU.
 * @return OK, or ERR_INVAL if `irq` is not an implemented SPI or the CPU has
 *         not initialized its GIC interface.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_spi_target(uint32_t irq, uint32_t target_cpu);

/**
 * @brief Assign an interrupt to Group 1 (non-secure IRQ).
 * Required on platforms where TF-A runs at EL3 and the kernel at EL1
 * can only receive Group 1 interrupts.
 * @param irq GIC interrupt ID (INTID).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_group1(uint32_t irq);

/**
 * @brief Configure an interrupt as level-triggered (default GIC reset value
 * may differ on real hardware vs QEMU).
 * @param irq GIC interrupt ID (INTID).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_level_triggered(uint32_t irq);

/**
 * @brief Configure an interrupt as edge-triggered.
 * @param irq GIC interrupt ID (INTID).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void set_edge_triggered(uint32_t irq);

} // namespace irq

#endif // STELLUX_AARCH64_IRQ_IRQ_ARCH_H
