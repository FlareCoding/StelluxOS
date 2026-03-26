#ifndef STELLUX_NET_ICMP_H
#define STELLUX_NET_ICMP_H

#include "common/types.h"
#include "net/net.h"

namespace net {

constexpr uint8_t ICMP_TYPE_ECHO_REPLY   = 0;
constexpr uint8_t ICMP_TYPE_ECHO_REQUEST = 8;

constexpr uint32_t ICMP_MAX_WAITERS = 8;

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;  // network byte order
    uint16_t id;        // network byte order
    uint16_t sequence;  // network byte order
} __attribute__((packed));

static_assert(sizeof(icmp_header) == 8, "icmp_header must be 8 bytes");

/**
 * Initialize the ICMP subsystem (ping waiter infrastructure).
 */
void icmp_init();

/**
 * Process a received ICMP packet (after IPv4 header is stripped).
 * @param iface  Interface the packet arrived on.
 * @param src_ip Source IP in HOST byte order.
 * @param data   ICMP packet data (starting with icmp_header).
 * @param len    Length of ICMP packet.
 */
void icmp_recv(netif* iface, uint32_t src_ip, const uint8_t* data, size_t len);

/**
 * Send an ICMP echo request (ping).
 * @param iface   Interface to send on.
 * @param dst_ip  Destination IP in HOST byte order.
 * @param id      ICMP identifier (host byte order).
 * @param seq     ICMP sequence number (host byte order).
 * @return 0 on success, negative error code on failure.
 */
int32_t icmp_send_echo_request(netif* iface, uint32_t dst_ip,
                               uint16_t id, uint16_t seq);

/**
 * Send an ICMP echo request and wait for the reply.
 * Blocks until a matching reply arrives or timeout expires.
 * @param iface      Interface to use.
 * @param dst_ip     Destination IP in HOST byte order.
 * @param id         ICMP identifier.
 * @param seq        ICMP sequence number.
 * @param timeout_ms Timeout in milliseconds.
 * @param out_rtt_us Receives round-trip time in microseconds on success.
 * @return 0 on success, ERR_TIMEOUT on timeout, other negative on error.
 */
int32_t icmp_ping(netif* iface, uint32_t dst_ip,
                  uint16_t id, uint16_t seq,
                  uint32_t timeout_ms, uint32_t* out_rtt_us);

} // namespace net

#endif // STELLUX_NET_ICMP_H
