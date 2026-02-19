#ifndef STELLUX_ARCH_AARCH64_SCHED_THREAD_CPU_CONTEXT_H
#define STELLUX_ARCH_AARCH64_SCHED_THREAD_CPU_CONTEXT_H

#include "common/types.h"

namespace sched {

struct thread_cpu_context {
    uint64_t x[31];   // x0-x30 (x30 = link register)
    uint64_t sp;
    uint64_t pc;      // Program counter (saved ELR)
    uint64_t pstate;  // Processor state (saved SPSR)
};

} // namespace sched

#endif // STELLUX_ARCH_AARCH64_SCHED_THREAD_CPU_CONTEXT_H
