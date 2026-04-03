#ifndef STELLUX_COMMON_TIME_UTIL_H
#define STELLUX_COMMON_TIME_UTIL_H

#include "common/types.h"

namespace time_util {

/**
 * @brief Convert a Gregorian date to Unix epoch seconds.
 * Uses the Linux mktime64 algorithm.
 * Unprivileged: pure arithmetic, no hardware access.
 * @param year  Full year (e.g. 2026)
 * @param mon   Month 1-12
 * @param day   Day of month 1-31
 * @param hour  Hour 0-23
 * @param min   Minute 0-59
 * @param sec   Second 0-59
 * @return Seconds since 1970-01-01 00:00:00 UTC.
 */
uint64_t date_to_unix(uint32_t year, uint32_t mon, uint32_t day,
                      uint32_t hour, uint32_t min, uint32_t sec);

} // namespace time_util

#endif // STELLUX_COMMON_TIME_UTIL_H
