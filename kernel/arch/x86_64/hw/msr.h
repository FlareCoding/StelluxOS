#ifndef STELLUX_ARCH_X86_64_HW_MSR_H
#define STELLUX_ARCH_X86_64_HW_MSR_H

#include "types.h"

namespace msr {

/**
 * Read a 64-bit Model Specific Register.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
inline uint64_t read(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

/**
 * Write a 64-bit Model Specific Register.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
inline void write(uint32_t msr, uint64_t value) {
    uint32_t lo = static_cast<uint32_t>(value);
    uint32_t hi = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

} // namespace msr

#endif // STELLUX_ARCH_X86_64_HW_MSR_H
