#ifndef STELLUX_ARCH_X86_64_HW_TSC_H
#define STELLUX_ARCH_X86_64_HW_TSC_H

#include "common/types.h"

namespace tsc {

inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline uint64_t rdtscp() {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

} // namespace tsc

#endif // STELLUX_ARCH_X86_64_HW_TSC_H
