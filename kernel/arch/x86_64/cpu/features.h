#ifndef STELLUX_ARCH_X86_64_CPU_FEATURES_H
#define STELLUX_ARCH_X86_64_CPU_FEATURES_H

#include "types.h"

namespace cpu {

// Feature flag constants
// These are Stellux-internal flags, not raw CPUID bits
constexpr uint64_t APIC       = 1ULL << 0;   // Local APIC
constexpr uint64_t PAT        = 1ULL << 1;   // Page Attribute Table
constexpr uint64_t PGE        = 1ULL << 2;   // Global pages (CR4.PGE)
constexpr uint64_t FSGSBASE   = 1ULL << 3;   // RDGSBASE/WRGSBASE instructions
constexpr uint64_t SMEP       = 1ULL << 4;   // Supervisor Mode Execution Prevention
constexpr uint64_t SMAP       = 1ULL << 5;   // Supervisor Mode Access Prevention
constexpr uint64_t NX         = 1ULL << 6;   // No-Execute bit (EFER.NXE)
constexpr uint64_t PAGE_1GB   = 1ULL << 7;   // 1GB huge pages
constexpr uint64_t LA57       = 1ULL << 8;   // 5-level paging
constexpr uint64_t PCID       = 1ULL << 9;   // Process Context Identifiers
constexpr uint64_t INVPCID    = 1ULL << 10;  // INVPCID instruction
constexpr uint64_t TSC        = 1ULL << 11;  // Time Stamp Counter
constexpr uint64_t TSC_DEADLINE = 1ULL << 12; // TSC deadline timer
constexpr uint64_t X2APIC     = 1ULL << 13;  // x2APIC mode
constexpr uint64_t XSAVE      = 1ULL << 14;  // XSAVE/XRSTOR
constexpr uint64_t AVX        = 1ULL << 15;  // AVX instructions
constexpr uint64_t AVX2       = 1ULL << 16;  // AVX2 instructions
constexpr uint64_t INVARIANT_TSC = 1ULL << 17; // Invariant TSC (constant rate across P/C states)
constexpr uint64_t RDTSCP     = 1ULL << 18;  // RDTSCP instruction
constexpr uint64_t RDRAND     = 1ULL << 19;  // RDRAND instruction (hardware RNG)
constexpr uint64_t RDSEED     = 1ULL << 20;  // RDSEED instruction (hardware entropy)

// Features required for Stellux to boot
constexpr uint64_t REQUIRED = FSGSBASE | NX | APIC | PAT;

struct features {
    uint64_t flags;
    uint8_t family;
    uint8_t model;
    uint8_t stepping;
};

__PRIVILEGED_DATA extern features g_features;

constexpr int32_t OK = 0;
constexpr int32_t ERR_REQUIRED_FEATURE = -1;

/**
 * Initialize CPU features: detect via CPUID, enable FSGSBASE and PAT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

// Check if CPU has all features in mask
inline bool has(uint64_t mask) {
    return (g_features.flags & mask) == mask;
}

} // namespace cpu

#endif // STELLUX_ARCH_X86_64_CPU_FEATURES_H
