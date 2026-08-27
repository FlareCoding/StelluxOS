#ifndef STELLUX_ARCH_X86_64_SCHED_FPU_H
#define STELLUX_ARCH_X86_64_SCHED_FPU_H

#include "sched/fpu_state.h"

namespace fpu {

// FXSAVE area layout: MXCSR and the CPU-reported mask of its usable bits
constexpr size_t MXCSR_OFFSET      = 24;
constexpr size_t MXCSR_MASK_OFFSET = 28;

// Architectural fallback when the CPU reports no mask, DAZ excluded
constexpr uint32_t MXCSR_DEFAULT_MASK = 0xFFBF;

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

    // FCW (offset 0): 0x037F - mask all x87 exceptions, 64-bit precision, round-to-nearest
    area[0] = 0x7F;
    area[1] = 0x03;

    // MXCSR: 0x1F80 - mask all SSE exceptions, round-to-nearest
    area[MXCSR_OFFSET]     = 0x80;
    area[MXCSR_OFFSET + 1] = 0x1F;
}

/**
 * @brief Clear MXCSR bits the CPU rejects from an untrusted FP image.
 * FXRSTOR raises #GP in Ring 0 on a reserved bit, so an image that crossed
 * the user boundary must pass through here before restore.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void sanitize_user_mxcsr(sched::fpu_state* state) {
    sched::fpu_state probe;
    save(&probe);

    uint32_t mask =
        *reinterpret_cast<const uint32_t*>(&probe.fxsave_area[MXCSR_MASK_OFFSET]);
    if (!mask) {
        mask = MXCSR_DEFAULT_MASK;
    }

    *reinterpret_cast<uint32_t*>(&state->fxsave_area[MXCSR_OFFSET]) &= mask;
}

} // namespace fpu

#endif // STELLUX_ARCH_X86_64_SCHED_FPU_H
