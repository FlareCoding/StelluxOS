#ifndef STELLUX_ARCH_AARCH64_TRAP_TRAP_FRAME_H
#define STELLUX_ARCH_AARCH64_TRAP_TRAP_FRAME_H

#include "types.h"

namespace aarch64 {

struct alignas(16) trap_frame {
    uint64_t x[31]; // x0-x30
    uint64_t sp;    // interrupted-context SP (EL0 traps store SP_EL0)
    uint64_t sp_el1; // EL1 exception stack pointer to use after trap return
    uint64_t elr;
    uint64_t spsr;
    uint64_t esr;
    uint64_t far;
    uint64_t _reserved0;
};

static_assert(__builtin_offsetof(trap_frame, x) == 0x00);
static_assert(__builtin_offsetof(trap_frame, sp) == 0xF8);
static_assert(__builtin_offsetof(trap_frame, sp_el1) == 0x100);
static_assert(__builtin_offsetof(trap_frame, elr) == 0x108);
static_assert(__builtin_offsetof(trap_frame, spsr) == 0x110);
static_assert(__builtin_offsetof(trap_frame, esr) == 0x118);
static_assert(__builtin_offsetof(trap_frame, far) == 0x120);
static_assert(sizeof(trap_frame) == 0x130);

inline uint64_t get_ip(const trap_frame* tf) {
    return tf->elr;
}

inline uint64_t get_sp(const trap_frame* tf) {
    return tf->sp;
}

inline bool from_user(const trap_frame* tf) {
    // SPSR.M[4:0] == 0b00000 => EL0t
    return (tf->spsr & 0x1F) == 0;
}

inline uint64_t get_esr(const trap_frame* tf) {
    return tf->esr;
}

inline uint64_t get_far(const trap_frame* tf) {
    return tf->far;
}

} // namespace aarch64

#endif // STELLUX_ARCH_AARCH64_TRAP_TRAP_FRAME_H

