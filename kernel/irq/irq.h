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
 * @brief Initialize the interrupt controller for an AP.
 * x86_64: enables this CPU's LAPIC (SVR), masks LVTs, clears EOI.
 *         Uses the shared LAPIC MMIO mapping from init().
 * AArch64: stub (GIC CPU interface already configured).
 * @return OK on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap();

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

// ============================================================================
// Generic IRQ handler dispatch
// ============================================================================

constexpr int32_t ERR_INVAL    = -3;
constexpr int32_t ERR_BUSY     = -4;
constexpr uint32_t MAX_IRQ     = 1024;

/**
 * Callback signature for registered IRQ handlers.
 * Called from interrupt context with interrupts acknowledged.
 * @param irq     The interrupt number that fired.
 * @param context Opaque pointer passed during registration.
 */
using irq_handler_fn = void (*)(uint32_t irq, void* context);

/**
 * Register an IRQ handler for a specific interrupt number.
 * Only one handler per IRQ is supported; returns ERR_BUSY if already
 * registered. The handler is called from interrupt context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_handler(uint32_t irq, irq_handler_fn fn,
                                           void* context);

/**
 * Unregister a previously registered IRQ handler.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unregister_handler(uint32_t irq);

/**
 * Dispatch an interrupt to a registered handler.
 * Called from the architecture trap handler. Returns true if a handler
 * was registered and invoked, false otherwise.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool dispatch(uint32_t irq);

} // namespace irq

#endif // STELLUX_IRQ_IRQ_H
