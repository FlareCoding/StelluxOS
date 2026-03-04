#ifndef STELLUX_CLOCK_CLOCK_H
#define STELLUX_CLOCK_CLOCK_H

#include "common/types.h"

namespace clock {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Initialize the monotonic clock subsystem.
 * x86_64: calibrates TSC frequency via PIT.
 * AArch64: reads CNTFRQ_EL0 and enables EL0 counter access.
 * After this call, now_ns() returns meaningful values.
 * Must be called after arch::early_init() and mm::init().
 * @return OK on success, ERR on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Per-AP clock initialization.
 * AArch64: enables EL0 counter access (CNTKCTL_EL1 is per-CPU).
 * Both: verifies BSP calibration data is visible on this AP.
 * Must be called on each AP before timer::init_ap().
 * @return OK on success, ERR if BSP calibration not visible.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap();

/**
 * @brief Monotonic nanoseconds since boot.
 * Lock-free, safe from any CPU and any privilege level (Ring 3 / EL0).
 * Returns 0 before init() has been called.
 */
uint64_t now_ns();

/**
 * @brief Counter frequency in Hz (for debug/logging).
 */
uint64_t freq_hz();

/**
 * @brief Unix epoch in nanoseconds at boot time.
 * Returns 0 if no RTC was available or rtc::init() was not called.
 * Unprivileged: reads a cached value from regular .bss.
 */
uint64_t boot_realtime_ns();

} // namespace clock

#endif // STELLUX_CLOCK_CLOCK_H
