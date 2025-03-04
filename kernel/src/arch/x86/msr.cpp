#ifdef ARCH_X86_64
#include <arch/x86/msr.h>
#include <arch/x86/cpuid.h>
#include <core/string.h>

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

__PRIVILEGED_CODE
int32_t read_cpu_temperature() {
    char cpu_vendor[24] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor);

    uint64_t msr_value = 0;

    // Intel CPU temperature reading (IA32_THERM_STATUS)
    if (strcmp(cpu_vendor, "GenuineIntel") == 0) {
        msr_value = read(IA32_THERM_STATUS);
        
        // Check if temperature sensor is enabled (Bit 31 must be set)
        if (!(msr_value & (1ULL << 31))) {
            return -1; // MSR not available
        }

        uint8_t temp_readout = (msr_value >> 16) & 0x7F;
        int32_t tj_max = 100; // Default TjMax (may vary by model)
        return tj_max - temp_readout;
    }

    // AMD CPU temperature reading (AMD THERMTRIP MSR)
    if (strcmp(cpu_vendor, "AuthenticAMD") == 0) {
        uint32_t cpu_family = cpuid_read_cpu_family();

        // AMD temperature MSR is only available on Family 10h (0x10) and newer
        if (cpu_family < 0x10) {
            return -1; // Temperature MSR is not available on older AMD CPUs
        }

        msr_value = read(AMD_THERMTRIP);

        uint8_t temp_readout = (msr_value >> 16) & 0x7F;
        return static_cast<int32_t>(temp_readout - 49); // AMD offset correction
    }

    return -1; // Unsupported CPU vendor
}
} // namespace arch::x86::msr
#endif // ARCH_X86_64

