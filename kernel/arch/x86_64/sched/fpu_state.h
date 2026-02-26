#ifndef STELLUX_ARCH_X86_64_SCHED_FPU_STATE_H
#define STELLUX_ARCH_X86_64_SCHED_FPU_STATE_H

#include "common/types.h"

namespace sched {

struct alignas(16) fpu_state {
    uint8_t fxsave_area[512];
};

} // namespace sched

#endif // STELLUX_ARCH_X86_64_SCHED_FPU_STATE_H
