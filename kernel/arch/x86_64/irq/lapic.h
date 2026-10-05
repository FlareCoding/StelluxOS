#ifndef STELLUX_ARCH_X86_64_IRQ_LAPIC_H
#define STELLUX_ARCH_X86_64_IRQ_LAPIC_H

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
constexpr uint32_t LAPIC_LVT_THERMAL  = 0x330;
constexpr uint32_t LAPIC_LVT_PERFCNT  = 0x340;
constexpr uint32_t LAPIC_LVT_LINT0    = 0x350;
constexpr uint32_t LAPIC_LVT_LINT1    = 0x360;
constexpr uint32_t LAPIC_LVT_ERROR    = 0x370;
constexpr uint32_t LAPIC_TIMER_ICR    = 0x380;
constexpr uint32_t LAPIC_TIMER_CCR    = 0x390;
constexpr uint32_t LAPIC_TIMER_DCR    = 0x3E0;

// LVT Timer register bits
constexpr uint32_t LVT_MASKED         = (1 << 16);
constexpr uint32_t LVT_PERIODIC       = (1 << 17);

/**
 * @brief Map the local APIC registers.
 * Must be called once on the boot CPU after acpi::init() and mm::init(), and
 * before any other LAPIC function.
 * @return OK, ERR_NO_MADT if the MADT gives no LAPIC address, or ERR_MAP if
 *         the registers could not be mapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_lapic();

/**
 * @brief Read a register of the calling CPU's local APIC.
 * @param offset One of the LAPIC_ register offsets.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t read_lapic_register(uint32_t offset);

/**
 * @brief Write a register of the calling CPU's local APIC.
 * @param offset One of the LAPIC_ register offsets.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void write_lapic_register(uint32_t offset, uint32_t value);

/**
 * @brief Get the mapped LAPIC virtual address.
 * Valid after irq::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t get_lapic_va();

} // namespace irq

#endif // STELLUX_ARCH_X86_64_IRQ_LAPIC_H
