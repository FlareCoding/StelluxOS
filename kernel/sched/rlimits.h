#ifndef STELLUX_SCHED_RLIMITS_H
#define STELLUX_SCHED_RLIMITS_H

#include "common/types.h"

namespace sched {

// POSIX resource limits at the index positions musl passes to the
// kernel, named only for the ones Stellux gives real values
constexpr uint32_t RLIMIT_STACK  = 3;
constexpr uint32_t RLIMIT_NOFILE = 7;
constexpr uint32_t RLIMIT_COUNT  = 16;

constexpr uint64_t RLIM_INFINITY = ~0ULL;

struct rlimit_pair {
    uint64_t soft;
    uint64_t hard;
};

/**
 * @brief Fill a limit array with process creation defaults.
 * Stack and handle table limits reflect the enforced kernel values,
 * every other resource is unlimited.
 */
void init_default_rlimits(rlimit_pair* limits);

} // namespace sched

#endif // STELLUX_SCHED_RLIMITS_H
