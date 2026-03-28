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
 * Compute UDP checksum over pseudo-header + UDP packet (RFC 768).
 * IP addresses must be in network byte order (read as byte arrays for
 * consistency with the LE 16-bit word sum used by inet_checksum).
 * Returns the raw one's complement: 0 means the data sums to all-ones.
 *
 * TX usage: if result is 0, transmit 0xFFFF (RFC 768 convention).
 * RX usage: pass full UDP including checksum field; result == 0 means valid.
 */
inline uint16_t udp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                              const uint8_t* udp_packet, size_t udp_len) {
    uint32_t sum = 0;

    // Pseudo-header IP addresses: read bytes of the network-order uint32_t
    // as LE 16-bit words (same convention as inet_checksum's data loop).
    const auto* s = reinterpret_cast<const uint8_t*>(&src_ip_net);
    const auto* d = reinterpret_cast<const uint8_t*>(&dst_ip_net);
    sum += static_cast<uint16_t>(s[0]) | (static_cast<uint16_t>(s[1]) << 8);
    sum += static_cast<uint16_t>(s[2]) | (static_cast<uint16_t>(s[3]) << 8);
    sum += static_cast<uint16_t>(d[0]) | (static_cast<uint16_t>(d[1]) << 8);
    sum += static_cast<uint16_t>(d[2]) | (static_cast<uint16_t>(d[3]) << 8);

    // Zero + protocol 17: network bytes [0x00, 0x11], LE word = 0x1100
    sum += static_cast<uint16_t>(0x1100);

    // UDP length in network byte order as LE word
    uint16_t ul = static_cast<uint16_t>(udp_len);
    sum += static_cast<uint16_t>((ul >> 8) | ((ul & 0xFF) << 8));

    // Sum the UDP header + data
    const uint8_t* ptr = udp_packet;
    size_t remaining = udp_len;
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

/**
 * Compute TCP checksum over pseudo-header + TCP segment (RFC 9293).
 * Identical to udp_checksum except protocol = 6.
 * IP addresses must be in network byte order.
 *
 * TX usage: if result is 0, transmit 0xFFFF.
 * RX usage: pass full TCP segment including checksum field, result == 0 means valid.
 */
inline uint16_t tcp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                              const uint8_t* tcp_segment, size_t tcp_len) {
    uint32_t sum = 0;

    const auto* s = reinterpret_cast<const uint8_t*>(&src_ip_net);
    const auto* d = reinterpret_cast<const uint8_t*>(&dst_ip_net);
    sum += static_cast<uint16_t>(s[0]) | (static_cast<uint16_t>(s[1]) << 8);
    sum += static_cast<uint16_t>(s[2]) | (static_cast<uint16_t>(s[3]) << 8);
    sum += static_cast<uint16_t>(d[0]) | (static_cast<uint16_t>(d[1]) << 8);
    sum += static_cast<uint16_t>(d[2]) | (static_cast<uint16_t>(d[3]) << 8);

    // Zero + protocol 6: network bytes [0x00, 0x06], LE word = 0x0600
    sum += static_cast<uint16_t>(0x0600);

    // TCP length in network byte order as LE word
    uint16_t tl = static_cast<uint16_t>(tcp_len);
    sum += static_cast<uint16_t>((tl >> 8) | ((tl & 0xFF) << 8));

    const uint8_t* ptr = tcp_segment;
    size_t remaining = tcp_len;
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

} // namespace net

#endif // STELLUX_NET_CHECKSUM_H
