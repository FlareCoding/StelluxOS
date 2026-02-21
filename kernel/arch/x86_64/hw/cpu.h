#ifndef STELLUX_ARCH_X86_64_HW_CPU_H
#define STELLUX_ARCH_X86_64_HW_CPU_H

#include "types.h"

namespace cpu {

__PRIVILEGED_CODE inline void halt() {
    asm volatile("hlt");
}

inline void relax() {
    asm volatile("pause");
}

inline void send_event() {
}

__PRIVILEGED_CODE inline void irq_disable() {
    asm volatile("cli" ::: "memory");
}

__PRIVILEGED_CODE inline void irq_enable() {
    asm volatile("sti" ::: "memory");
}

__PRIVILEGED_CODE inline uint64_t irq_save() {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

__PRIVILEGED_CODE inline void irq_restore(uint64_t flags) {
    asm volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

} // namespace cpu

#endif // STELLUX_ARCH_X86_64_HW_CPU_H
