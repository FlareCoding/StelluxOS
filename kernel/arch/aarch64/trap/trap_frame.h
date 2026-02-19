#ifndef STELLUX_ARCH_AARCH64_TRAP_TRAP_FRAME_H
#define STELLUX_ARCH_AARCH64_TRAP_TRAP_FRAME_H

#include "types.h"

namespace aarch64 {

struct alignas(16) trap_frame {
    uint64_t x[31]; // x0-x30
    uint64_t sp;    // interrupted-context SP (EL0 traps store SP_EL0)
    uint64_t elr;
    uint64_t spsr;
    uint64_t esr;
    uint64_t far;
};

static_assert(sizeof(trap_frame) == 0x120);

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

