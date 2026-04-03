#ifndef STELLUX_ARCH_AARCH64_SCHED_FPU_STATE_H
#define STELLUX_ARCH_AARCH64_SCHED_FPU_STATE_H

#include "common/types.h"

namespace sched {

struct alignas(16) fpu_state {
    uint8_t  vregs[32][16]; // V0-V31 (128-bit each)
    uint32_t fpcr;
    uint32_t fpsr;
};

} // namespace sched

#endif // STELLUX_ARCH_AARCH64_SCHED_FPU_STATE_H
