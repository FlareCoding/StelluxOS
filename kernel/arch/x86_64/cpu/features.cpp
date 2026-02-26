#include "cpu/features.h"
#include "hw/msr.h"

namespace cpu {

__PRIVILEGED_DATA features g_features = {};

// CPUID wrapper
__PRIVILEGED_CODE
static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t* eax, uint32_t* ebx,
                         uint32_t* ecx, uint32_t* edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

// CPUID leaf 1 EDX bits
constexpr uint32_t CPUID_1_EDX_TSC    = 1 << 4;
constexpr uint32_t CPUID_1_EDX_APIC   = 1 << 9;
constexpr uint32_t CPUID_1_EDX_PGE    = 1 << 13;
constexpr uint32_t CPUID_1_EDX_PAT    = 1 << 16;

// CPUID leaf 1 ECX bits
constexpr uint32_t CPUID_1_ECX_X2APIC      = 1 << 21;
constexpr uint32_t CPUID_1_ECX_TSC_DEADLINE = 1 << 24;
constexpr uint32_t CPUID_1_ECX_XSAVE       = 1 << 26;
constexpr uint32_t CPUID_1_ECX_AVX         = 1 << 28;
constexpr uint32_t CPUID_1_ECX_PCID        = 1 << 17;

// CPUID leaf 7 EBX bits
constexpr uint32_t CPUID_7_EBX_FSGSBASE = 1 << 0;
constexpr uint32_t CPUID_7_EBX_SMEP     = 1 << 7;
constexpr uint32_t CPUID_7_EBX_AVX2     = 1 << 5;
constexpr uint32_t CPUID_7_EBX_SMAP     = 1 << 20;
constexpr uint32_t CPUID_7_EBX_INVPCID  = 1 << 10;

// CPUID extended leaf 0x80000001 EDX bits
constexpr uint32_t CPUID_EXT1_EDX_NX      = 1 << 20;
constexpr uint32_t CPUID_EXT1_EDX_PAGE1GB = 1 << 26;
constexpr uint32_t CPUID_EXT1_EDX_RDTSCP  = 1 << 27;

// CPUID extended leaf 0x80000007 EDX bits
constexpr uint32_t CPUID_EXT7_EDX_INVARIANT_TSC = 1 << 8;

// CPUID leaf 7 ECX bits
constexpr uint32_t CPUID_7_ECX_LA57 = 1 << 16;

// Detect CPU features via CPUID and populate g_features
__PRIVILEGED_CODE static void detect() {
    uint32_t eax, ebx, ecx, edx;
    g_features.flags = 0;

    // Leaf 0: Get max standard leaf and vendor
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;

    if (max_leaf < 1) {
        return; // ancient CPU, no features
    }

    // Leaf 1: Basic features + family/model/stepping
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    g_features.stepping = eax & 0x0F;
    uint32_t base_model = (eax >> 4) & 0x0F;
    uint32_t base_family = (eax >> 8) & 0x0F;
    uint32_t ext_model = (eax >> 16) & 0x0F;
    uint32_t ext_family = (eax >> 20) & 0xFF;

    if (base_family == 0x0F) {
        g_features.family = static_cast<uint8_t>(base_family + ext_family);
    } else {
        g_features.family = static_cast<uint8_t>(base_family);
    }

    if (base_family == 0x06 || base_family == 0x0F) {
        g_features.model = static_cast<uint8_t>((ext_model << 4) | base_model);
    } else {
        g_features.model = static_cast<uint8_t>(base_model);
    }

    // Leaf 1 EDX features
    if (edx & CPUID_1_EDX_TSC)  g_features.flags |= TSC;
    if (edx & CPUID_1_EDX_APIC) g_features.flags |= APIC;
    if (edx & CPUID_1_EDX_PGE)  g_features.flags |= PGE;
    if (edx & CPUID_1_EDX_PAT)  g_features.flags |= PAT;

    // Leaf 1 ECX features
    if (ecx & CPUID_1_ECX_PCID)         g_features.flags |= PCID;
    if (ecx & CPUID_1_ECX_X2APIC)       g_features.flags |= X2APIC;
    if (ecx & CPUID_1_ECX_TSC_DEADLINE) g_features.flags |= TSC_DEADLINE;
    if (ecx & CPUID_1_ECX_XSAVE)        g_features.flags |= XSAVE;
    if (ecx & CPUID_1_ECX_AVX)          g_features.flags |= AVX;

    // Leaf 7: Extended features
    if (max_leaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);

        if (ebx & CPUID_7_EBX_FSGSBASE) g_features.flags |= FSGSBASE;
        if (ebx & CPUID_7_EBX_SMEP)     g_features.flags |= SMEP;
        if (ebx & CPUID_7_EBX_SMAP)     g_features.flags |= SMAP;
        if (ebx & CPUID_7_EBX_AVX2)     g_features.flags |= AVX2;
        if (ebx & CPUID_7_EBX_INVPCID)  g_features.flags |= INVPCID;
        if (ecx & CPUID_7_ECX_LA57)     g_features.flags |= LA57;
    }

    // Extended leaf 0x80000001: AMD features and NX
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext_leaf = eax;

    if (max_ext_leaf >= 0x80000001) {
        cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);

        if (edx & CPUID_EXT1_EDX_NX)      g_features.flags |= NX;
        if (edx & CPUID_EXT1_EDX_PAGE1GB) g_features.flags |= PAGE_1GB;
        if (edx & CPUID_EXT1_EDX_RDTSCP)  g_features.flags |= RDTSCP;
    }

    if (max_ext_leaf >= 0x80000007) {
        cpuid(0x80000007, 0, &eax, &ebx, &ecx, &edx);

        if (edx & CPUID_EXT7_EDX_INVARIANT_TSC) g_features.flags |= INVARIANT_TSC;
    }
}

// Enable FSGSBASE instructions via CR4.FSGSBASE (bit 16)
__PRIVILEGED_CODE static void enable_fsgsbase() {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 16);
    asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

// PAT MSR and memory type encodings
constexpr uint32_t MSR_IA32_PAT = 0x277;
constexpr uint8_t PAT_UC  = 0x00; // Uncacheable
constexpr uint8_t PAT_WC  = 0x01; // Write-Combining
constexpr uint8_t PAT_WT  = 0x04; // Write-Through
constexpr uint8_t PAT_WB  = 0x06; // Write-Back
constexpr uint8_t PAT_UCM = 0x07; // UC-minus

// Linux-compatible PAT layout:
// PAT0=WB, PAT1=WC, PAT2=UC-, PAT3=UC, PAT4=WB, PAT5=WC, PAT6=UC-, PAT7=WT
// PTE selection: PAT1 (WC) = PWT=1, PCD=0, PAT=0
constexpr uint64_t PAT_VALUE =
    (static_cast<uint64_t>(PAT_WB)  << 0)  |
    (static_cast<uint64_t>(PAT_WC)  << 8)  |
    (static_cast<uint64_t>(PAT_UCM) << 16) |
    (static_cast<uint64_t>(PAT_UC)  << 24) |
    (static_cast<uint64_t>(PAT_WB)  << 32) |
    (static_cast<uint64_t>(PAT_WC)  << 40) |
    (static_cast<uint64_t>(PAT_UCM) << 48) |
    (static_cast<uint64_t>(PAT_WT)  << 56);

// Initialize PAT with Write-Combining support
__PRIVILEGED_CODE static void init_pat() {
    msr::write(MSR_IA32_PAT, PAT_VALUE);
}

__PRIVILEGED_CODE int32_t init() {
    detect();

    // Check required features
    if ((g_features.flags & REQUIRED) != REQUIRED) {
        return ERR_REQUIRED_FEATURE;
    }

    // Enable FSGSBASE
    enable_fsgsbase();

    // Initialize PAT with WC support
    init_pat();

    return OK;
}

} // namespace cpu
