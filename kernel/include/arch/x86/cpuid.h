#ifdef ARCH_X86_64
#ifndef CPUID_H
#define CPUID_H
#include <memory/memory.h>

// Basic CPUID Information
#define CPUID_VENDOR_ID            0x00000000
#define CPUID_FEATURES             0x00000001
#define CPUID_CACHE_DESC           0x00000002
#define CPUID_SERIAL_NUMBER        0x00000003

// Extended CPUID Information
#define CPUID_EXTENDED_FEATURES    0x80000001
#define CPUID_BRAND_STRING_1       0x80000002
#define CPUID_BRAND_STRING_2       0x80000003
#define CPUID_BRAND_STRING_3       0x80000004
#define CPUID_CACHE_INFO           0x80000006

// Feature bits in EDX for CPUID with EAX=1
#define CPUID_FEAT_EDX_PAE         (1 << 6)
#define CPUID_FEAT_EDX_APIC        (1 << 9)
#define CPUID_FEAT_EDX_PGE         (1 << 13)
#define CPUID_FEAT_EDX_PAT         (1 << 16)

// Feature bits in ECX for CPUID with EAX=1
#define CPUID_FEAT_ECX_SSE3        (1 << 0)
#define CPUID_FEAT_ECX_VMX         (1 << 5)

// Feature bits in ECX for CPUID with EAX=7, ECX=0
#define CPUID_FEAT_ECX_FSGSBASE    (1 << 0)
#define CPUID_FEAT_ECX_LA57        (1 << 16)  // 5-level paging

// SSE (Streaming SIMD Extensions) Feature Bit
#define CPUID_EDX_SSE         0x02000000

// SSE2 (Streaming SIMD Extensions 2) Feature Bit
#define CPUID_EDX_SSE2        0x04000000

// SSE3 (Streaming SIMD Extensions 3) Feature Bit
#define CPUID_ECX_SSE3        0x00000001

// AVX (Advanced Vector Extensions) Feature Bit
#define CPUID_ECX_AVX         0x10000000

// FMA3 (Fused Multiply-Add 3) Feature Bit
#define CPUID_ECX_FMA         0x00001000

namespace arch::x86 {
/**
 * @brief Reads basic CPUID information for a given code.
 * @param code The CPUID code to query.
 * @param a Pointer to store the EAX register result.
 * @param d Pointer to store the EDX register result.
 * 
 * Uses the CPUID instruction to retrieve information based on the provided code.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void read_cpuid(
    int code,
    uint32_t* a,
    uint32_t* d
) {
    __asm__ volatile("cpuid"
        : "=a"(*a), "=d"(*d)  // Output operands
        : "0"(code)           // Input operands
        : "ecx", "ebx"        // Clobbered registers
    );
}

/**
 * @brief Reads extended CPUID information for a given code.
 * @param code The CPUID code to query.
 * @param a Pointer to store the EAX register result.
 * @param d Pointer to store the EDX register result.
 * 
 * Uses the CPUID instruction to query extended information.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void read_cpuid_extended(
    int code,
    uint32_t* a,
    uint32_t* d
) {
    __asm__ volatile("cpuid"
        : "=a"(*a), "=d"(*d)  // Output operands
        : "0"(code)           // Input operands
        : "ecx", "ebx"        // Clobbered registers
    );
}

/**
 * @brief Reads full CPUID information for a given code.
 * @param code The CPUID code to query.
 * @param a Pointer to store the EAX register result.
 * @param b Pointer to store the EBX register result.
 * @param c Pointer to store the ECX register result.
 * @param d Pointer to store the EDX register result.
 * 
 * Retrieves all register values (EAX, EBX, ECX, EDX) using the CPUID instruction.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void read_cpuid_full(
    int code,
    uint32_t* a,
    uint32_t* b,
    uint32_t* c,
    uint32_t* d
) {
    __asm__ volatile("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) // Output operands
        : "0"(code)                              // Input operand
    );
}

/**
 * @brief Executes the CPUID instruction with the given leaf and subleaf.
 * 
 * @param leaf The main CPUID function to query.
 * @param subleaf The subfunction to query (for Intel 0x04, AMD 0x8000001D).
 * @param eax Pointer to store the EAX register result.
 * @param ebx Pointer to store the EBX register result.
 * @param ecx Pointer to store the ECX register result.
 * @param edx Pointer to store the EDX register result.
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void read_cpuid_full(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t* eax,
    uint32_t* ebx,
    uint32_t* ecx,
    uint32_t* edx
) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

/**
 * @brief Checks if 5-level page tables (LA57) are supported.
 * @return True if LA57 is supported, false otherwise.
 * 
 * Uses the CPUID instruction to determine 57-bit addressing support.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline int cpuid_is_la57_supported() {
    uint32_t a, d;
    read_cpuid(7, &a, &d);
    return (a & CPUID_FEAT_ECX_LA57) != 0;
}

/**
 * @brief Reads the CPU vendor ID into a buffer.
 * @param vendor Pointer to a buffer of at least 13 bytes to store the vendor ID string.
 * 
 * Queries the vendor ID string using the CPUID instruction and null-terminates it.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline void cpuid_read_vendor_id(char* vendor) {
    uint32_t ebx, ecx, edx;
    asm volatile("cpuid":"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(CPUID_VENDOR_ID));
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';
}

/**
 * @brief Reads the CPU family ID.
 * @return CPU family ID.
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline uint32_t cpuid_read_cpu_family() {
    uint32_t eax, ebx, ecx, edx;
    asm volatile ("cpuid":"=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(CPUID_FEATURES));

    uint32_t base_family = (eax >> 8) & 0xF;
    uint32_t extended_family = (eax >> 20) & 0xFF;
    uint32_t family = (base_family == 0xF) ? (base_family + extended_family) : base_family;

    return family;
}

/**
 * @brief Reads the CPU model ID.
 * @return CPU model ID.
 */
__PRIVILEGED_CODE static inline uint32_t cpuid_read_cpu_model() {
    uint32_t eax, ebx, ecx, edx;
    read_cpuid_full(CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    return (eax >> 4) & 0xF; // Bits [7:4] contain the model ID
}

/**
 * @brief Reads the CPU stepping ID.
 * @return CPU stepping ID.
 */
__PRIVILEGED_CODE static inline uint32_t cpuid_read_cpu_stepping() {
    uint32_t eax, ebx, ecx, edx;
    read_cpuid_full(CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    return eax & 0xF; // Bits [3:0] contain the stepping ID
}

/**
 * @brief Reads the CPU brand string.
 * @param brand Pointer to a buffer of at least 49 bytes to store the brand string.
 */
__PRIVILEGED_CODE static inline void cpuid_read_cpu_brand(char* brand) {
    uint32_t data[12];
    
    read_cpuid_full(CPUID_BRAND_STRING_1, &data[0], &data[1], &data[2], &data[3]);
    read_cpuid_full(CPUID_BRAND_STRING_2, &data[4], &data[5], &data[6], &data[7]);
    read_cpuid_full(CPUID_BRAND_STRING_3, &data[8], &data[9], &data[10], &data[11]);

    memcpy(brand, data, 48);
    brand[48] = '\0';
}

/**
 * @brief Reads the number of logical CPU cores.
 * @return The number of logical CPU cores.
 */
__PRIVILEGED_CODE static inline uint32_t cpuid_read_logical_cores() {
    uint32_t eax, ebx, ecx, edx;
    read_cpuid_full(CPUID_FEATURES, &eax, &ebx, &ecx, &edx);
    return (ebx >> 16) & 0xFF; // Bits [23:16] of EBX contain the logical processor count
}

/**
 * @brief Reads the number of physical CPU cores.
 * @return The number of physical CPU cores.
 */
__PRIVILEGED_CODE uint32_t cpuid_read_physical_cores();

/**
 * @brief Reads L1, L2, and L3 cache sizes in KB.
 * @param l1 Pointer to store L1 cache size.
 * @param l2 Pointer to store L2 cache size.
 * @param l3 Pointer to store L3 cache size.
 */
__PRIVILEGED_CODE void cpuid_read_cache_sizes(uint32_t* l1, uint32_t* l2, uint32_t* l3);

/**
 * @brief Checks if the CPU supports SSE instructions.
 * @return True if SSE is supported, false otherwise.
 * 
 * Queries the CPUID features leaf to check for SSE support.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_sse_supported() {
    uint32_t eax, edx;
    read_cpuid(CPUID_FEATURES, &eax, &edx);
    return (edx & CPUID_EDX_SSE) != 0;
}

/**
 * @brief Checks if the CPU supports SSE2 instructions.
 * @return True if SSE2 is supported, false otherwise.
 * 
 * Queries the CPUID features leaf to check for SSE2 support.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_sse2_supported() {
    uint32_t eax, edx;
    read_cpuid(CPUID_FEATURES, &eax, &edx);
    return (edx & CPUID_EDX_SSE2) != 0;
}

/**
 * @brief Checks if the CPU supports SSE3 instructions.
 * @return True if SSE3 is supported, false otherwise.
 * 
 * Queries the CPUID features leaf to check for SSE3 support.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_sse3_supported() {
    uint32_t eax, edx;
    read_cpuid(CPUID_FEATURES, &eax, &edx);
    return (eax & CPUID_ECX_SSE3) != 0;
}

/**
 * @brief Checks if the CPU supports AVX instructions.
 * @return True if AVX is supported, false otherwise.
 * 
 * Queries the CPUID features leaf to check for AVX support.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_avx_supported() {
    uint32_t eax, edx;
    read_cpuid(CPUID_FEATURES, &eax, &edx);
    return (eax & CPUID_ECX_AVX) != 0;
}

/**
 * @brief Checks if the CPU supports FMA instructions.
 * @return True if FMA is supported, false otherwise.
 * 
 * Queries the CPUID features leaf to check for FMA support.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_fma_supported() {
    uint32_t eax, edx;
    read_cpuid(CPUID_FEATURES, &eax, &edx);
    return (eax & CPUID_ECX_FMA) != 0;
}


/**
 * @brief Checks if the CPU supports the Page Attribute Table (PAT).
 * @return True if PAT is supported, false otherwise.
 * 
 * Queries the CPUID features leaf to check for PAT support.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_pat_supported() {
    uint32_t eax, edx;
    read_cpuid(CPUID_FEATURES, &eax, &edx);
    return (edx & CPUID_FEAT_EDX_PAT) != 0;
}

/**
 * @brief Checks if the CPU supports the FSGSBASE instruction set.
 * 
 * This function uses the CPUID instruction to determine whether the processor supports the
 * FSGSBASE instructions. These instructions allow direct access to the FS and GS segment
 * base registers without needing privileged system calls. Enabling FSGSBASE can improve
 * performance in low-level context switching and threading operations.
 * 
 * @return True if FSGSBASE is supported, false otherwise.
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_fsgsbase_supported() {
    uint32_t eax, ebx, ecx, edx;
    
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(7), "c"(0)); // CPUID leaf 7, subleaf 0

    return (ebx & (1 << 0)); // Check bit 0 of EBX for FSGSBASE support
}

/**
 * @brief Checks if the CPU is running under QEMU or KVM.
 * @return True if running under QEMU/KVM, false otherwise.
 * 
 * Uses the CPUID instruction to retrieve the hypervisor vendor signature and compares it
 * with known signatures for QEMU and KVM.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static inline bool cpuid_is_running_under_qemu() {
    uint32_t eax, ebx, ecx, edx;

    // Call CPUID with leaf 0x40000000
    read_cpuid_full(0x40000000, &eax, &ebx, &ecx, &edx);

    // The hypervisor signature is stored in ebx, ecx, and edx
    char hypervisorSignature[13];
    ((uint32_t *)hypervisorSignature)[0] = ebx;
    ((uint32_t *)hypervisorSignature)[1] = ecx;
    ((uint32_t *)hypervisorSignature)[2] = edx;
    hypervisorSignature[12] = '\0';  // Null-terminate the string

    // Check if the signature matches "TCGTCGTCGTCG" or "KVMKVMKVM\0\0\0"
    if (memcmp(hypervisorSignature, (void*)"TCGTCGTCGTCG", 12) == 0 || memcmp(hypervisorSignature, (void*)"KVMKVMKVM\0\0\0", 12) == 0) {
        return true;  // The system is running under QEMU or KVM
    }

    return false;  // Not running under QEMU/KVM
}
} // namespace arch::x86

#endif // CPUID_H
#endif // ARCH_X86_64
