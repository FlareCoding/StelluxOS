#ifndef STELLUX_ARCH_X86_64_HW_RNG_H
#define STELLUX_ARCH_X86_64_HW_RNG_H

#include "cpu/features.h"

namespace hw::rng {

constexpr uint32_t RETRY_LIMIT = 10;

inline bool rdrand64(uint64_t* out) {
    uint8_t ok;
    asm volatile("rdrandq %0; setc %1" : "=r"(*out), "=qm"(ok) :: "cc");
    return ok;
}

inline bool available() {
    return cpu::has(cpu::RDRAND);
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
            if (rdrand64(&val)) {
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

#endif // STELLUX_ARCH_X86_64_HW_RNG_H
