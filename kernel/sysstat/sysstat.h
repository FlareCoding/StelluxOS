#ifndef STELLUX_SYSSTAT_SYSSTAT_H
#define STELLUX_SYSSTAT_SYSSTAT_H

#include "common/types.h"

namespace sysstat {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Register the /dev/sysinfo nodes with devfs.
 *
 * Each node is a readable text file that captures a fresh snapshot
 * when read from offset zero and serves that snapshot to sequential
 * reads until the reader returns to offset zero:
 *
 *   /dev/sysinfo/cpu     tick_hz, then one "cpu<N> <busy> <idle>" per CPU
 *   /dev/sysinfo/mem     page_size, total_pages, free_pages, used_pages
 *   /dev/sysinfo/uptime  monotonic nanoseconds since boot
 *   /dev/sysinfo/tasks   one "tid pid state cpu ticks name" line per task
 *
 * Must be called after devfs is mounted.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace sysstat

#endif // STELLUX_SYSSTAT_SYSSTAT_H
