#ifndef STELLUX_ARCH_AARCH64_HW_DELAY_H
#define STELLUX_ARCH_AARCH64_HW_DELAY_H

#include "common/types.h"
#include "hw/cpu.h"
#include "clock/clock.h"

namespace delay {

/**
 * Busy-wait for the given number of nanoseconds using CNTVCT_EL0.
 * Requires clock::init() to have been called.
 */
inline void ns(uint64_t n) {
    if (n == 0) return;
    uint64_t start = clock::now_ns();
    while (clock::now_ns() - start < n) {
        cpu::relax();
    }
}

/**
 * Busy-wait for the given number of microseconds using CNTVCT_EL0.
 * Requires clock::init() to have been called.
 */
inline void us(uint64_t u) {
    ns(u * 1000);
}

} // namespace delay

#endif // STELLUX_ARCH_AARCH64_HW_DELAY_H
