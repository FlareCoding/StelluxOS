/*
 * Hash function utilities.
 *
 * Two families: golden-ratio multiplicative hashing for integers/pointers
 * (fast, one multiply), and FNV-1a for byte sequences and strings.
 * Full hash producers return full-width values; the caller (or hash map)
 * folds to a bucket index via masking.
 *
 * Usage:
 *   uint64_t h = hash::u64(my_key);
 *   uint64_t s = hash::string("hello");
 *   uint64_t c = hash::combine(hash::ptr(parent), hash::string(name));
 *
 * Thread safety: all functions are pure (no shared state).
 */

#ifndef STELLUX_COMMON_HASH_H
#define STELLUX_COMMON_HASH_H

#include "types.h"

namespace hash {

constexpr uint64_t GOLDEN_RATIO_64 = 0x61C8864680B583EBull;
constexpr uint32_t GOLDEN_RATIO_32 = 0x61C88647u;

constexpr uint64_t FNV_OFFSET_64 = 0xcbf29ce484222325ull;
constexpr uint64_t FNV_PRIME_64  = 0x100000001b3ull;

// Hash a 64-bit integer. Returns full 64-bit hash.
inline uint64_t u64(uint64_t val) {
    return val * GOLDEN_RATIO_64;
}

// Hash a 32-bit integer. Returns full 32-bit hash.
inline uint32_t u32(uint32_t val) {
    return val * GOLDEN_RATIO_32;
}

// Hash a pointer. Returns full 64-bit hash.
inline uint64_t ptr(const void* p) {
    return u64(reinterpret_cast<uintptr_t>(p));
}

// Hash a byte sequence (FNV-1a). Returns full 64-bit hash.
inline uint64_t bytes(const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = FNV_OFFSET_64;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= FNV_PRIME_64;
    }
    return h;
}

// Hash a null-terminated string (FNV-1a). Returns full 64-bit hash.
inline uint64_t string(const char* s) {
    uint64_t h = FNV_OFFSET_64;
    while (*s) {
        h ^= static_cast<uint8_t>(*s++);
        h *= FNV_PRIME_64;
    }
    return h;
}

// Fold a 64-bit hash to the top `bits` bits. Precondition: 1 <= bits <= 32.
inline uint32_t fold64(uint64_t hash, uint32_t bits) {
    return static_cast<uint32_t>(hash >> (64 - bits));
}

// Combine two hash values. Non-commutative: combine(a,b) != combine(b,a).
// Based on 64-bit golden-ratio constant (2^64 / phi).
inline uint64_t combine(uint64_t seed, uint64_t hash) {
    return seed ^ (hash + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
}

} // namespace hash

#endif // STELLUX_COMMON_HASH_H
