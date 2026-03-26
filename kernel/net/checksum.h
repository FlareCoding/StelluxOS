#ifndef STELLUX_NET_CHECKSUM_H
#define STELLUX_NET_CHECKSUM_H

#include "common/types.h"

namespace net {

/**
 * Compute RFC 1071 internet checksum (one's complement sum).
 * Used by IPv4 and ICMP headers.
 */
inline uint16_t inet_checksum(const void* data, size_t len) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;

    // Sum 16-bit words
    while (len > 1) {
        uint16_t word = static_cast<uint16_t>(ptr[0]) |
                        (static_cast<uint16_t>(ptr[1]) << 8);
        sum += word;
        ptr += 2;
        len -= 2;
    }

    // Add odd byte if present
    if (len == 1) {
        sum += static_cast<uint16_t>(ptr[0]);
    }

    // Fold 32-bit sum into 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

} // namespace net

#endif // STELLUX_NET_CHECKSUM_H
