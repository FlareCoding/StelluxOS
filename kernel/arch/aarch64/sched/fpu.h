#ifndef STELLUX_ARCH_AARCH64_SCHED_FPU_H
#define STELLUX_ARCH_AARCH64_SCHED_FPU_H

#include "sched/fpu_state.h"

extern "C" {
void fpu_save_state(sched::fpu_state* state);
void fpu_restore_state(const sched::fpu_state* state);
}

namespace fpu {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void save(sched::fpu_state* s) {
    fpu_save_state(s);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void restore(const sched::fpu_state* s) {
    fpu_restore_state(s);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void init_state(sched::fpu_state* s) {
    auto* p = reinterpret_cast<uint8_t*>(s);
    for (size_t i = 0; i < sizeof(sched::fpu_state); i++) {
        p[i] = 0;
    }
}

} // namespace fpu

#endif // STELLUX_ARCH_AARCH64_SCHED_FPU_H
