#ifndef CPUID_H
#define CPUID_H
#include <ktypes.h>

// Basic CPUID Information
#define CPUID_VENDOR_ID            0x00000000
#define CPUID_FEATURES             0x00000001
#define CPUID_CACHE_DESC           0x00000002
#define CPUID_SERIAL_NUMBER        0x00000003

// Extended CPUID Information
#define CPUID_EXTENDED_FEATURES    0x80000001

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

// Read basic CPUID information
__PRIVILEGED_CODE
static inline void readCpuid(int code, uint32_t *a, uint32_t *d) {
    __asm__ volatile("cpuid"
        : "=a"(*a), "=d"(*d)  // Output operands
        : "0"(code)           // Input operands
        : "ecx", "ebx"        // Clobbered registers
    );
}

// Read extended CPUID information
__PRIVILEGED_CODE
static inline void readCpuidExtended(int code, uint32_t *a, uint32_t *d) {
    __asm__ volatile("cpuid"
        : "=a"(*a), "=d"(*d)  // Output operands
        : "0"(code)           // Input operands
        : "ecx", "ebx"        // Clobbered registers
    );
}

// Returns whether or not 5-level page tables are supported
__PRIVILEGED_CODE
static inline int cpuid_isLa57Supported() {
    uint32_t a, d;
    readCpuid(7, &a, &d);
    return (a & CPUID_FEAT_ECX_LA57) != 0;
}

// Reads the Vendor ID into the specified buffer.
// *Note: vendor buffer should be at least 13 bytes in size
__PRIVILEGED_CODE
static inline void cpuid_readVendorId(char* vendor) {
    uint32_t ebx, ecx, edx;
    asm volatile("cpuid":"=b"(ebx),"=c"(ecx),"=d"(edx):"a"(CPUID_VENDOR_ID));
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';
}

__PRIVILEGED_CODE
static inline bool cpuid_isSSESupported() {
    uint32_t eax, edx;
    readCpuid(CPUID_FEATURES, &eax, &edx);
    return (edx & CPUID_EDX_SSE) != 0;
}

__PRIVILEGED_CODE
static inline bool cpuid_isSSE2Supported() {
    uint32_t eax, edx;
    readCpuid(CPUID_FEATURES, &eax, &edx);
    return (edx & CPUID_EDX_SSE2) != 0;
}

__PRIVILEGED_CODE
static inline bool cpuid_isSSE3Supported() {
    uint32_t eax, edx;
    readCpuid(CPUID_FEATURES, &eax, &edx);
    return (eax & CPUID_ECX_SSE3) != 0;
}

__PRIVILEGED_CODE
static inline bool cpuid_isAVXSupported() {
    uint32_t eax, edx;
    readCpuid(CPUID_FEATURES, &eax, &edx);
    return (eax & CPUID_ECX_AVX) != 0;
}

__PRIVILEGED_CODE
static inline bool cpuid_isFMASupported() {
    uint32_t eax, edx;
    readCpuid(CPUID_FEATURES, &eax, &edx);
    return (eax & CPUID_ECX_FMA) != 0;
}

__PRIVILEGED_CODE
static inline bool cpuid_isPATSupported() {
    uint32_t eax, edx;
    readCpuid(CPUID_FEATURES, &eax, &edx);
    return (edx & CPUID_FEAT_EDX_PAT) != 0;
}

#endif
