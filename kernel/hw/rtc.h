#ifndef STELLUX_HW_RTC_H
#define STELLUX_HW_RTC_H

#include "common/types.h"

namespace rtc {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Read the hardware RTC and cache the boot-time Unix epoch.
 * x86_64: reads CMOS RTC via port I/O.
 * AArch64/QEMU: reads PL031 RTC via MMIO.
 * AArch64/RPi4: uses build-time epoch fallback.
 * Must be called after acpi::init() and mm::init().
 * @return OK on success, ERR on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Unix epoch in nanoseconds captured at boot.
 * Returns 0 if init() has not been called.
 * Unprivileged: reads a cached value from regular .bss.
 */
uint64_t boot_unix_ns();

} // namespace rtc

#endif // STELLUX_HW_RTC_H
