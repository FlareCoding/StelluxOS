#ifdef ARCH_X86_64
#include <arch/x86/cpuid.h>
#include <string.h>

namespace arch::x86 {
__PRIVILEGED_CODE
uint32_t cpuid_read_physical_cores() {
    char cpu_vendor[24] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor);
    
    uint32_t eax, ebx, ecx, edx;
    uint32_t core_count = 1; // Default fallback to 1 core

    if (strcmp(cpu_vendor, "GenuineIntel") == 0) {
        // Check if CPUID 0x04 is supported
        read_cpuid_full(0x00000000, 0, &eax, &ebx, &ecx, &edx);
        if (eax >= 0x04) {
            // Modern Intel CPUs: Use CPUID 0x04
            read_cpuid_full(0x04, 0, &eax, &ebx, &ecx, &edx);
            core_count = ((eax >> 26) & 0x3F) + 1; // Extract cores per package
        } 
        else if (eax >= 0x0B) {
            // Use CPUID 0x0B for Extended Topology Enumeration
            uint32_t level = 0;
            do {
                read_cpuid_full(0x0B, level, &eax, &ebx, &ecx, &edx);
                if ((ecx & 0xFF) == 1) {
                    core_count = ebx & 0xFFFF;
                    break;
                }
                level++;
            } while (eax != 0);
        }
        else if (eax >= 0x01) {
            // Fallback for legacy Intel CPUs (CPUID 0x01)
            read_cpuid_full(0x01, 0, &eax, &ebx, &ecx, &edx);
            uint32_t logical_count = (ebx >> 16) & 0xFF; // Logical cores in package
            bool has_hyper_threading = edx & (1 << 28); // Check Hyper-Threading flag
            core_count = has_hyper_threading ? (logical_count / 2) : logical_count;
        }
    } 
    else if (strcmp(cpu_vendor, "AuthenticAMD") == 0) {
        // Check if CPUID 0x8000001E is supported
        read_cpuid_full(0x80000000, 0, &eax, &ebx, &ecx, &edx);
        if (eax >= 0x8000001E) {
            // Modern AMD CPUs: Use CPUID 0x8000001E
            read_cpuid_full(0x8000001E, 0, &eax, &ebx, &ecx, &edx);
            core_count = (ebx & 0xFF) + 1;
        } 
        else if (eax >= 0x80000008) {
            // Older AMD CPUs: Use CPUID 0x80000008
            read_cpuid_full(0x80000008, 0, &eax, &ebx, &ecx, &edx);
            core_count = (ecx & 0xFF) + 1;
        }
    }

    return core_count;
}

__PRIVILEGED_CODE
void cpuid_read_cache_sizes(uint32_t* l1, uint32_t* l2, uint32_t* l3) {
    char cpu_vendor[24] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor);

    *l1 = *l2 = *l3 = 0; // Default to 0 if not found

    if (strcmp(cpu_vendor, "GenuineIntel") == 0) {
        // Intel Modern CPUs: Use CPUID 0x04 (Cache Parameters)
        uint32_t eax, ebx, ecx, edx;
        uint32_t cache_type, cache_level;
        uint32_t i = 0;

        do {
            read_cpuid_full(0x04, i, &eax, &ebx, &ecx, &edx);
            cache_type = eax & 0x1F;
            if (cache_type == 0) break; // No more caches

            cache_level = (eax >> 5) & 0x7;
            uint32_t line_size = (ebx & 0xFFF) + 1;
            uint32_t partitions = ((ebx >> 12) & 0x3FF) + 1;
            uint32_t ways = ((ebx >> 22) & 0x3FF) + 1;
            uint32_t sets = ecx + 1;
            uint32_t cache_size = ways * partitions * line_size * sets;

            switch (cache_level) {
                case 1: *l1 += cache_size; break;
                case 2: *l2 += cache_size; break;
                case 3: *l3 += cache_size; break;
            }
            i++;
        } while (cache_type != 0);

        // Intel Older CPUs: Use CPUID 0x02 (Cache Descriptor Lookup)
        if (*l1 == 0 && *l2 == 0 && *l3 == 0) {
            read_cpuid_full(0x02, 0, &eax, &ebx, &ecx, &edx);
            // This method requires a lookup table for cache descriptors
        }

    } else if (strcmp(cpu_vendor, "AuthenticAMD") == 0) {
        uint32_t eax, ebx, ecx, edx;

        // AMD Modern CPUs: Use CPUID 0x8000001D (Cache Properties)
        uint32_t cache_type, cache_level;
        uint32_t i = 0;

        do {
            read_cpuid_full(0x8000001D, i, &eax, &ebx, &ecx, &edx);
            cache_type = eax & 0x1F;
            if (cache_type == 0) break;

            cache_level = (eax >> 5) & 0x7;
            uint32_t line_size = (ebx & 0xFFF) + 1;
            uint32_t partitions = ((ebx >> 12) & 0x3FF) + 1;
            uint32_t ways = ((ebx >> 22) & 0x3FF) + 1;
            uint32_t sets = ecx + 1;
            uint32_t cache_size = ways * partitions * line_size * sets;

            switch (cache_level) {
                case 1: *l1 += cache_size; break;
                case 2: *l2 += cache_size; break;
                case 3: *l3 += cache_size; break;
            }
            i++;
        } while (cache_type != 0);

        // AMD Older CPUs: Use CPUID 0x80000005 & 0x80000006
        if (*l1 == 0) {
            read_cpuid_full(0x80000005, 0, &eax, &ebx, &ecx, &edx);
            *l1 = ((ecx >> 24) & 0xFF) * 1024; // L1 Data
            *l1 += ((edx >> 24) & 0xFF) * 1024; // L1 Instruction
        }
        if (*l2 == 0 || *l3 == 0) {
            read_cpuid_full(0x80000006, 0, &eax, &ebx, &ecx, &edx);
            *l2 = ((ecx >> 16) & 0xFFFF) * 1024; // L2
            *l3 = ((edx >> 18) & 0x3FFF) * 512 * 1024; // L3 (512KB units)
        }
    } else {
        // Unknown CPU Vendor: Use fallback lookup table (not implemented)
    }
}
} // namespace arch::x86
#endif // ARCH_X86_64

