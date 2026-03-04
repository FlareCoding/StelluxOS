#include "common/time_util.h"

namespace time_util {

uint64_t date_to_unix(uint32_t year, uint32_t mon, uint32_t day,
                      uint32_t hour, uint32_t min, uint32_t sec) {
    uint32_t y = year;
    uint32_t m = mon;

    // Shift Jan/Feb to months 11/12 of the previous year (puts Feb last
    // so leap day doesn't split the year boundary in the formula).
    if (m <= 2) {
        m += 10;
        y -= 1;
    } else {
        m -= 2;
    }

    int64_t days = static_cast<int64_t>(y / 4 - y / 100 + y / 400 +
                   367 * m / 12 + day) +
                   static_cast<int64_t>(y) * 365 - 719499;

    return static_cast<uint64_t>(((days * 24 + hour) * 60 + min) * 60 + sec);
}

} // namespace time_util
