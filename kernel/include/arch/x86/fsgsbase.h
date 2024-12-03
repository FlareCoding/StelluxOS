#ifndef FSGSBASE_H
#define FSGSBASE_H
#ifdef ARCH_X86_64
#include <types.h>

namespace arch::x86 {
static __force_inline__ void enable_fsgsbase() {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 16); // Set the FSGSBASE bit
    asm volatile("mov %0, %%cr4" :: "r"(cr4));
}

static __force_inline__ uint64_t rdfsbase() {
    uint64_t base;
    asm volatile("rdfsbase %0" : "=r"(base));
    return base;
}

static __force_inline__ void wrfsbase(uint64_t base) {
    asm volatile("wrfsbase %0" :: "r"(base));
}

static __force_inline__ uint64_t rdgsbase() {
    uint64_t base;
    asm volatile("rdgsbase %0" : "=r"(base));
    return base;
}

static __force_inline__ void wrgsbase(uint64_t base) {
    asm volatile("wrgsbase %0" :: "r"(base));
}

static __force_inline__ void swapgs() {
    asm volatile ("swapgs");
}
} // namespace arch::x86

#endif // ARCH_X86_64
#endif // FSGSBASE_H

