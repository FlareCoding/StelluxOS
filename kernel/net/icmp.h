#ifndef STELLUX_NET_ICMP_H
#define STELLUX_NET_ICMP_H

#include "common/types.h"
#include "net/net.h"

namespace net {

constexpr uint8_t ICMP_TYPE_ECHO_REPLY   = 0;
constexpr uint8_t ICMP_TYPE_ECHO_REQUEST = 8;

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;  // network byte order
    uint16_t id;        // network byte order
    uint16_t sequence;  // network byte order
} __attribute__((packed));

static_assert(sizeof(icmp_header) == 8, "icmp_header must be 8 bytes");

struct inet_socket;

/**
 * Process a received ICMP packet (after IPv4 header is stripped).
 * Handles echo requests (kernel replies via deferred TX) and delivers
 * all ICMP packets to registered sockets.
 */
void icmp_recv(netif* iface, uint32_t src_ip, const uint8_t* data, size_t len);

/**
 * Register an inet socket to receive ICMP packets.
 * Called during ICMP socket creation.
 */
void icmp_register_socket(inet_socket* sock);

/**
 * Unregister an inet socket from ICMP delivery.
 * Called during ICMP socket close.
 */
void icmp_unregister_socket(inet_socket* sock);

} // namespace net

#endif // STELLUX_NET_ICMP_H
