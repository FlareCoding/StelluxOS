#include "msr.h"

uint64_t readMsr(
    uint32_t msr
) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)low | ((uint64_t)high << 32));
}

void writeMsr(
    uint32_t msr,
    uint64_t value
) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}
