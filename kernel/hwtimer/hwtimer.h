#ifndef STELLUX_HWTIMER_HWTIMER_H
#define STELLUX_HWTIMER_HWTIMER_H

#include "common/types.h"

namespace hwtimer {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Initialize and start the hardware timer at the given frequency.
 * x86_64: calibrates LAPIC timer via PIT, configures periodic mode.
 * AArch64: reads CNTFRQ_EL0, configures Generic Timer, unmasks PPI 27.
 * Enables CPU interrupts after starting the timer.
 * Must be called after irq::init() and sched::init().
 * @param hz Tick frequency in Hertz (e.g., 100 for 10ms quantum).
 * @return OK on success, ERR on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uint32_t hz);

/**
 * @brief Stop the hardware timer and mask its interrupt.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void stop();

} // namespace hwtimer

#endif // STELLUX_HWTIMER_HWTIMER_H
