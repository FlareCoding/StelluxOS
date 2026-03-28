#ifndef STELLUX_NET_INET_SOCKET_H
#define STELLUX_NET_INET_SOCKET_H

#include "common/types.h"
#include "resource/resource.h"
#include "sync/spinlock.h"

struct ring_buffer;

namespace net {

// Kernel representation of sockaddr_in (matches Linux/musl layout).
// Used by sendto/recvfrom syscall handlers and inet socket ops.
struct kernel_sockaddr_in {
    uint16_t sin_family; // AF_INET = 2
    uint16_t sin_port;   // network byte order
    uint32_t sin_addr;   // network byte order
    uint8_t  sin_zero[8];
};

constexpr uint16_t AF_INET_VAL = 2;

struct inet_socket {
    uint8_t        protocol;   // e.g. IPPROTO_ICMP (1), IPPROTO_UDP (17)
    uint32_t       bound_addr; // 0 = any (host byte order)
    uint16_t       bound_port; // host byte order, 0 = unbound (UDP)
    ring_buffer*   rx_buf;     // incoming packets queued here
    uint32_t       so_options; // bitmask of socket options
    sync::spinlock lock;
    inet_socket*   next;       // linked list for protocol registry
};

/**
 * Create an AF_INET SOCK_DGRAM IPPROTO_ICMP socket.
 * Registers the socket with the ICMP protocol layer for packet delivery.
 */
int32_t create_inet_icmp_socket(resource::resource_object** out);

/**
 * Create an AF_INET SOCK_DGRAM IPPROTO_UDP socket.
 * Port registration is deferred until the first sendto assigns an ephemeral port.
 */
int32_t create_inet_udp_socket(resource::resource_object** out);

} // namespace net

#endif // STELLUX_NET_INET_SOCKET_H
