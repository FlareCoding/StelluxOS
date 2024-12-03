#ifdef ARCH_X86_64
#include <arch/x86/msr.h>

namespace arch::x86::msr {
__PRIVILEGED_CODE
uint64_t read(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)low | ((uint64_t)high << 32));
}

__PRIVILEGED_CODE
void write(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}
} // namespace arch::x86::msr
#endif // ARCH_X86_64

