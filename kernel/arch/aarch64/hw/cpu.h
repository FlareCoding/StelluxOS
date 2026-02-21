#ifndef STELLUX_ARCH_AARCH64_HW_CPU_H
#define STELLUX_ARCH_AARCH64_HW_CPU_H

#include "types.h"

namespace cpu {

inline void halt() {
    asm volatile("wfi");
}

inline void relax() {
    asm volatile("wfe");
}

inline void send_event() {
    asm volatile("sev" ::: "memory");
}

__PRIVILEGED_CODE inline void irq_disable() {
    asm volatile("msr daifset, #0xf" ::: "memory");
}

__PRIVILEGED_CODE inline void irq_enable() {
    asm volatile("msr daifclr, #0xf" ::: "memory");
}

__PRIVILEGED_CODE inline uint64_t irq_save() {
    uint64_t daif;
    asm volatile("mrs %0, daif; msr daifset, #0xf" : "=r"(daif) :: "memory");
    return daif;
}

__PRIVILEGED_CODE inline void irq_restore(uint64_t daif) {
    asm volatile("msr daif, %0" :: "r"(daif) : "memory");
}

} // namespace cpu

#endif // STELLUX_ARCH_AARCH64_HW_CPU_H
