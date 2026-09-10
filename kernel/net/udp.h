#ifndef STELLUX_NET_UDP_H
#define STELLUX_NET_UDP_H

#include "net/ipv4.h"

namespace net {
namespace udp {

constexpr size_t HEADER_LEN = 8;

// Ports handed to a socket that sends without binding (RFC 6335 dynamic range)
constexpr uint16_t EPHEMERAL_PORT_MIN = 49152;
constexpr uint16_t EPHEMERAL_PORT_MAX = 65535;

// A sender that skips the checksum stores zero, and one that computes a zero
// stores all ones so the two cases stay distinguishable (RFC 768).
constexpr uint16_t CHECKSUM_NONE     = 0;
constexpr uint16_t CHECKSUM_ALL_ONES = 0xFFFF;

/**
 * UDP header (RFC 768). The ports name the sockets at each end, `length`
 * counts the header and the payload, and `checksum` covers both plus a
 * pseudo-header of the IPv4 addresses, protocol, and length. A zero checksum
 * means none was computed, which an IPv4 receiver must accept.
 * https://www.rfc-editor.org/info/rfc768/
 */
struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));
static_assert(sizeof(udp_header) == HEADER_LEN);

/*
 * Consumes a datagram whose window starts at the UDP header. A valid datagram
 * goes to the bound socket, an unclaimed port is answered with ICMP port unreachable.
 */
int32_t input(packet* pkt);

/*
 * Consumes a finished payload with headroom, prepends the header for `src_port`
 * to `dest_port`, fills in the checksum, and hands it to IPv4 for `dest`.
 */
int32_t output(packet* pkt, const ipv4::ipv4_addr& dest, uint16_t src_port, uint16_t dest_port);

} // namespace udp
} // namespace net

#endif // STELLUX_NET_UDP_H
