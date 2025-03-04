#ifdef ARCH_X86_64
#include <arch/x86/cpuid.h>
#include <string.h>

namespace arch::x86 {
__PRIVILEGED_CODE
uint32_t cpuid_read_physical_cores() {
    char cpu_vendor[24] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor);

    if (strcmp(cpu_vendor, "GenuineIntel") == 0) {
        // Intel: Use CPUID Leaf 0x4, EAX[31:26] (Core count per package - 1)
        uint32_t eax, ebx, ecx, edx;
        read_cpuid_full(0x4, &eax, &ebx, &ecx, &edx);
        return ((eax >> 26) & 0x3F) + 1;
    }

    if (strcmp(cpu_vendor, "AuthenticAMD") == 0) {
        // AMD: Use CPUID Leaf 0x80000008, ECX[7:0] (Core count per processor)
        uint32_t eax, ebx, ecx, edx;
        read_cpuid_full(0x80000008, &eax, &ebx, &ecx, &edx);
        return (ecx & 0xFF) + 1;
    }

    return 1; // Default to 1 core if vendor is unknown
}

__PRIVILEGED_CODE
void cpuid_read_cache_sizes(uint32_t* l1, uint32_t* l2, uint32_t* l3) {
    char cpu_vendor[24] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor);

    *l1 = *l2 = *l3 = 0;

    if (strcmp(cpu_vendor, "GenuineIntel") == 0) {
        // Intel: Use CPUID Leaf 0x4 to iterate over cache levels
        uint32_t eax, ebx, ecx, edx;
        uint32_t cache_type, cache_level;
        uint32_t i = 0;

        do {
            read_cpuid_full(0x4, &eax, &ebx, &ecx, &edx);
            cache_type = eax & 0x1F;
            if (cache_type == 0) {
                break; // No more caches
            }

            cache_level = (eax >> 5) & 0x7;
            uint32_t line_size = (ebx & 0xFFF) + 1;
            uint32_t partitions = ((ebx >> 12) & 0x3FF) + 1;
            uint32_t ways = ((ebx >> 22) & 0x3FF) + 1;
            uint32_t sets = ecx + 1;
            uint32_t cache_size = ways * partitions * line_size * sets;

            switch (cache_level) {
                case 1:
                    *l1 += cache_size;
                    break;
                case 2:
                    *l2 += cache_size;
                    break;
                case 3:
                    *l3 += cache_size;
                    break;
                default:
                    break;
            }
            i++;
        } while (cache_type != 0);
    } else if (strcmp(cpu_vendor, "AuthenticAMD") == 0) {
        // AMD: Use CPUID Leaf 0x80000005 for L1, 0x80000006 for L2 & L3
        uint32_t eax, ebx, ecx, edx;

        // L1 Cache (Data + Instruction)
        read_cpuid_full(0x80000005, &eax, &ebx, &ecx, &edx);
        *l1 = ((ecx >> 24) & 0xFF) * 1024; // L1D (Data Cache)
        *l1 += ((edx >> 24) & 0xFF) * 1024; // L1I (Instruction Cache)

        // L2 and L3 Caches
        read_cpuid_full(0x80000006, &eax, &ebx, &ecx, &edx);
        *l2 = ((ecx >> 16) & 0xFFFF) * 1024; // L2 Cache
        *l3 = ((edx >> 18) & 0x3FFF) * 512 * 1024; // L3 Cache (512 KB units)
    }
}
} // namespace arch::x86
#endif // ARCH_X86_64

