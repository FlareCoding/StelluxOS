#ifndef STELLUX_DEBUG_LEB128_H
#define STELLUX_DEBUG_LEB128_H

#include "common/types.h"

namespace leb128 {

inline uint64_t read_uleb128(const uint8_t*& p, const uint8_t* end) {
    uint64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do {
        if (p >= end) return result;
        byte = *p++;
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if (shift >= 70) break;
    } while (byte & 0x80);
    return result;
}

inline int64_t read_sleb128(const uint8_t*& p, const uint8_t* end) {
    uint64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do {
        if (p >= end) return static_cast<int64_t>(result);
        byte = *p++;
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if (shift >= 70) break;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) {
        result |= ~static_cast<uint64_t>(0) << shift;
    }
    return static_cast<int64_t>(result);
}

} // namespace leb128

#endif // STELLUX_DEBUG_LEB128_H
