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

/**
 * Compute transport-layer checksum over pseudo-header + payload.
 * Used by both UDP (protocol=17) and TCP (protocol=6).
 * IP addresses must be in network byte order.
 *
 * TX usage: if result is 0, transmit 0xFFFF.
 * RX usage: pass full segment including checksum field; result == 0 means valid.
 */
inline uint16_t transport_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                                   uint8_t protocol,
                                   const uint8_t* payload, size_t payload_len) {
    uint32_t sum = 0;

    const auto* s = reinterpret_cast<const uint8_t*>(&src_ip_net);
    const auto* d = reinterpret_cast<const uint8_t*>(&dst_ip_net);
    sum += static_cast<uint16_t>(s[0]) | (static_cast<uint16_t>(s[1]) << 8);
    sum += static_cast<uint16_t>(s[2]) | (static_cast<uint16_t>(s[3]) << 8);
    sum += static_cast<uint16_t>(d[0]) | (static_cast<uint16_t>(d[1]) << 8);
    sum += static_cast<uint16_t>(d[2]) | (static_cast<uint16_t>(d[3]) << 8);

    // Zero + protocol as LE word: network bytes [0x00, proto]
    sum += static_cast<uint16_t>(protocol) << 8;

    // Payload length in network byte order as LE word
    uint16_t pl = static_cast<uint16_t>(payload_len);
    sum += static_cast<uint16_t>((pl >> 8) | ((pl & 0xFF) << 8));

    const uint8_t* ptr = payload;
    size_t remaining = payload_len;
    while (remaining > 1) {
        uint16_t word = static_cast<uint16_t>(ptr[0]) |
                        (static_cast<uint16_t>(ptr[1]) << 8);
        sum += word;
        ptr += 2;
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += static_cast<uint16_t>(ptr[0]);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

inline uint16_t udp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                              const uint8_t* udp_packet, size_t udp_len) {
    return transport_checksum(src_ip_net, dst_ip_net, 17, udp_packet, udp_len);
}

inline uint16_t tcp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                              const uint8_t* tcp_segment, size_t tcp_len) {
    return transport_checksum(src_ip_net, dst_ip_net, 6, tcp_segment, tcp_len);
}

} // namespace net

#endif // STELLUX_NET_CHECKSUM_H
