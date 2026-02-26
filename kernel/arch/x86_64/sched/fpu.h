#ifndef STELLUX_ARCH_X86_64_SCHED_FPU_H
#define STELLUX_ARCH_X86_64_SCHED_FPU_H

#include "sched/fpu_state.h"

namespace fpu {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void save(sched::fpu_state* state) {
    asm volatile("fxsave64 %0" : "=m"(state->fxsave_area) :: "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void restore(const sched::fpu_state* state) {
    asm volatile("fxrstor64 %0" :: "m"(state->fxsave_area) : "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void init_state(sched::fpu_state* state) {
    auto* area = state->fxsave_area;
    for (size_t i = 0; i < 512; i++) {
        area[i] = 0;
    }
    // FCW (offset 0): 0x037F — mask all x87 exceptions, 64-bit precision, round-to-nearest
    area[0] = 0x7F;
    area[1] = 0x03;
    // MXCSR (offset 24): 0x1F80 — mask all SSE exceptions, round-to-nearest
    area[24] = 0x80;
    area[25] = 0x1F;
}

} // namespace fpu

#endif // STELLUX_ARCH_X86_64_SCHED_FPU_H
