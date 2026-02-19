#ifndef STELLUX_ARCH_AARCH64_CPU_FEATURES_H
#define STELLUX_ARCH_AARCH64_CPU_FEATURES_H

#include "types.h"

namespace cpu {

// Feature flag constants derived from ID_AA64* registers
// These are Stellux-internal flags, not raw register bits
constexpr uint64_t FP        = 1ULL << 0;   // Floating point
constexpr uint64_t ASIMD     = 1ULL << 1;   // Advanced SIMD (NEON)
constexpr uint64_t ATOMICS   = 1ULL << 2;   // LSE atomics (FEAT_LSE)
constexpr uint64_t CRC32     = 1ULL << 3;   // CRC32 instructions
constexpr uint64_t SHA1      = 1ULL << 4;   // SHA1 crypto
constexpr uint64_t SHA256    = 1ULL << 5;   // SHA256 crypto
constexpr uint64_t AES       = 1ULL << 6;   // AES crypto
constexpr uint64_t PMULL     = 1ULL << 7;   // Polynomial multiply long
constexpr uint64_t RNG       = 1ULL << 8;   // Random number generator (FEAT_RNG)
constexpr uint64_t BTI       = 1ULL << 9;   // Branch Target Identification
constexpr uint64_t MTE       = 1ULL << 10;  // Memory Tagging Extension
constexpr uint64_t SVE       = 1ULL << 11;  // Scalable Vector Extension
constexpr uint64_t SVE2      = 1ULL << 12;  // SVE2

struct features {
    uint64_t flags;
    uint8_t implementer;  // MIDR_EL1[31:24]
    uint16_t part_num;    // MIDR_EL1[15:4]
    uint8_t variant;      // MIDR_EL1[23:20]
    uint8_t revision;     // MIDR_EL1[3:0]
};

__PRIVILEGED_DATA extern features g_features;

constexpr int32_t OK = 0;

/**
 * Initialize CPU features: detect via ID registers.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

// Check if CPU has all features in mask
inline bool has(uint64_t mask) {
    return (g_features.flags & mask) == mask;
}

} // namespace cpu

#endif // STELLUX_ARCH_AARCH64_CPU_FEATURES_H
