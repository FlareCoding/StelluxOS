#ifndef STELLUX_NET_UDP_H
#define STELLUX_NET_UDP_H

#include "common/types.h"
#include "net/net.h"

namespace net {

constexpr uint16_t UDP_PORT_EPHEMERAL_MIN = 49152;
constexpr uint16_t UDP_PORT_EPHEMERAL_MAX = 65535;

struct udp_header {
    uint16_t src_port;  // network byte order
    uint16_t dst_port;  // network byte order
    uint16_t length;    // network byte order (header + payload)
    uint16_t checksum;  // network byte order (0 = not computed)
} __attribute__((packed));

static_assert(sizeof(udp_header) == 8, "udp_header must be 8 bytes");

struct inet_socket;

/**
 * Process a received UDP packet (after IPv4 header is stripped).
 * Validates header, optionally verifies checksum, and delivers
 * the payload to the matching socket by destination port.
 * @param src_ip Source IP in host byte order.
 * @param dst_ip Destination IP in host byte order (for checksum verification).
 */
void udp_recv(netif* iface, uint32_t src_ip, uint32_t dst_ip,
              const uint8_t* data, size_t len);

/**
 * Register an inet socket to receive UDP packets on its bound_port.
 * Called when the socket is first assigned a port.
 */
void udp_register_socket(inet_socket* sock);

/**
 * Unregister an inet socket from UDP delivery.
 * Called during socket close.
 */
void udp_unregister_socket(inet_socket* sock);

/**
 * Allocate an ephemeral port number (49152-65535).
 * Thread-safe via atomic counter.
 * @return Port number in host byte order.
 */
uint16_t udp_alloc_ephemeral_port();

} // namespace net

#endif // STELLUX_NET_UDP_H
