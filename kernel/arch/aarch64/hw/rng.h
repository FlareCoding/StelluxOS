#ifndef STELLUX_ARCH_AARCH64_HW_RNG_H
#define STELLUX_ARCH_AARCH64_HW_RNG_H

#include "types.h"
#include "cpu/features.h"

namespace hw::rng {

constexpr uint32_t RETRY_LIMIT = 10;

// Uses the architectural encoding s3_3_c2_c4_0 rather than the named
// "rndr" mnemonic because the toolchain may not recognize it unless
// compiled with -march=armv8.5-a+rng or later.
inline bool rndr64(uint64_t* out) {
    uint64_t val;
    uint32_t ok;
    asm volatile("mrs %0, s3_3_c2_c4_0\n\t"
                 "cset %w1, ne"
                 : "=r"(val), "=r"(ok) :: "cc");
    *out = val;
    return ok != 0;
}

inline bool available() {
    return cpu::has(cpu::RNG);
}

inline bool fill(void* buf, size_t len) {
    if (!available()) {
        return false;
    }

    auto* dst = static_cast<uint8_t*>(buf);
    size_t offset = 0;

    while (offset < len) {
        uint64_t val = 0;
        bool ok = false;
        for (uint32_t retry = 0; retry < RETRY_LIMIT; retry++) {
            if (rndr64(&val)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return false;
        }

        size_t remaining = len - offset;
        size_t chunk = remaining < sizeof(val) ? remaining : sizeof(val);
        auto* src = reinterpret_cast<const uint8_t*>(&val);
        for (size_t i = 0; i < chunk; i++) {
            dst[offset + i] = src[i];
        }
        offset += chunk;
    }

    return true;
}

} // namespace hw::rng

#endif // STELLUX_ARCH_AARCH64_HW_RNG_H
