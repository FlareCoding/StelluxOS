#ifndef STELLUX_AARCH64_HWTIMER_HWTIMER_ARCH_H
#define STELLUX_AARCH64_HWTIMER_HWTIMER_ARCH_H

#include "common/types.h"

namespace hwtimer {

constexpr uint32_t TIMER_PPI = 27; // Virtual timer PPI (GIC interrupt ID)

inline uint32_t read_cntfrq() {
    uint64_t v;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return static_cast<uint32_t>(v);
}

inline void write_cntv_tval(uint32_t val) {
    asm volatile("msr CNTV_TVAL_EL0, %0" : : "r"(static_cast<uint64_t>(val)));
}

inline void write_cntv_ctl(uint32_t val) {
    asm volatile("msr CNTV_CTL_EL0, %0" : : "r"(static_cast<uint64_t>(val)));
}

/**
 * @brief Re-arm the Generic Timer for the next tick.
 * Must be called from the IRQ handler on each timer interrupt.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void rearm();

} // namespace hwtimer

#endif // STELLUX_AARCH64_HWTIMER_HWTIMER_ARCH_H
