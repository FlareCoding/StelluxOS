#ifndef STELLUX_X86_64_IRQ_IOAPIC_H
#define STELLUX_X86_64_IRQ_IOAPIC_H

#include "common/types.h"

namespace ioapic {

constexpr int32_t OK        = 0;
constexpr int32_t ERR_MAP   = -1;
constexpr int32_t ERR_NONE  = -2; // no IOAPIC in MADT
constexpr int32_t ERR_RANGE = -3; // GSI out of this IOAPIC's range

/**
 * @brief Initialize the first IOAPIC from MADT.
 * Maps MMIO registers into kernel VA. Must be called after
 * acpi::init() and mm::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Route a legacy ISA IRQ through the IOAPIC.
 * Checks ISOs for GSI remapping and polarity/trigger overrides.
 * Returns ERR_NONE if no IOAPIC was initialized, ERR_RANGE if the
 * resolved GSI falls outside this IOAPIC's entry range.
 * @param legacy_irq ISA IRQ number (e.g. 4 for COM1).
 * @param vector IDT vector to deliver (e.g. 0x24).
 * @param dest_apic_id Target LAPIC ID (0 for BSP).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t route_irq(uint8_t legacy_irq, uint8_t vector,
                                    uint8_t dest_apic_id);

} // namespace ioapic

#endif // STELLUX_X86_64_IRQ_IOAPIC_H
