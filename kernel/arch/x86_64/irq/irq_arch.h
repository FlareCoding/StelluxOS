#ifndef STELLUX_X86_64_IRQ_IRQ_ARCH_H
#define STELLUX_X86_64_IRQ_IRQ_ARCH_H

#include "common/types.h"

namespace irq {

// LAPIC MMIO register offsets
constexpr uint32_t LAPIC_ID           = 0x020;
constexpr uint32_t LAPIC_VERSION      = 0x030;
constexpr uint32_t LAPIC_EOI          = 0x0B0;
constexpr uint32_t LAPIC_SVR          = 0x0F0;
constexpr uint32_t LAPIC_ICR_LOW      = 0x300;
constexpr uint32_t LAPIC_ICR_HIGH     = 0x310;
constexpr uint32_t LAPIC_LVT_TIMER    = 0x320;
constexpr uint32_t LAPIC_TIMER_ICR    = 0x380;
constexpr uint32_t LAPIC_TIMER_CCR    = 0x390;
constexpr uint32_t LAPIC_TIMER_DCR    = 0x3E0;

// LVT Timer register bits
constexpr uint32_t LVT_MASKED         = (1 << 16);
constexpr uint32_t LVT_PERIODIC       = (1 << 17);

/**
 * @brief Get the mapped LAPIC virtual address.
 * Valid after irq::init(). Used by the timer layer to access
 * LAPIC timer registers in the same 4KB MMIO page.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t get_lapic_va();

} // namespace irq

#endif // STELLUX_X86_64_IRQ_IRQ_ARCH_H
