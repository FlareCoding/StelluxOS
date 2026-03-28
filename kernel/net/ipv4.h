#ifndef STELLUX_NET_IPV4_H
#define STELLUX_NET_IPV4_H

#include "common/types.h"
#include "net/net.h"

namespace net {

constexpr uint8_t IPV4_PROTO_ICMP = 1;
constexpr uint8_t IPV4_PROTO_TCP  = 6;
constexpr uint8_t IPV4_PROTO_UDP  = 17;

constexpr uint8_t IPV4_DEFAULT_TTL = 64;

struct ipv4_header {
    uint8_t  ver_ihl;     // version(4) + IHL(4)
    uint8_t  tos;
    uint16_t total_len;   // network byte order
    uint16_t id;          // network byte order
    uint16_t flags_frag;  // network byte order
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;    // network byte order
    uint32_t src_ip;      // network byte order
    uint32_t dst_ip;      // network byte order
} __attribute__((packed));

static_assert(sizeof(ipv4_header) == 20, "ipv4_header must be 20 bytes");

/**
 * Helper to construct an IPv4 address from 4 octets (host byte order).
 */
inline constexpr uint32_t ipv4_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) <<  8) |
           static_cast<uint32_t>(d);
}

/**
 * Process a received IPv4 packet (after Ethernet header is stripped).
 * Validates header, checksum, and dispatches to the appropriate protocol handler.
 */
void ipv4_recv(netif* iface, const uint8_t* data, size_t len);

/**
 * Send an IPv4 packet.
 * Builds the IP header, resolves the next-hop MAC via ARP, and sends via Ethernet.
 * @param iface    Interface to send on.
 * @param dst_ip   Destination IP in HOST byte order.
 * @param protocol IP protocol number (e.g., IPV4_PROTO_ICMP).
 * @param payload  Payload data.
 * @param payload_len Length of payload.
 * @param src_ip_override Source IP in HOST byte order, or 0 to let
 *        routing decide. When non-zero, this address is stamped into
 *        the IPv4 header instead of the route-derived source.
 * @return 0 on success, negative error code on failure.
 */
int32_t ipv4_send(netif* iface, uint32_t dst_ip, uint8_t protocol,
                  const uint8_t* payload, size_t payload_len,
                  uint32_t src_ip_override = 0);

} // namespace net

#endif // STELLUX_NET_IPV4_H
