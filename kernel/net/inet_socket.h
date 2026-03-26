#ifndef STELLUX_NET_INET_SOCKET_H
#define STELLUX_NET_INET_SOCKET_H

#include "common/types.h"
#include "resource/resource.h"
#include "sync/spinlock.h"

struct ring_buffer;

namespace net {

constexpr uint32_t MAX_ICMP_SOCKETS = 8;

struct inet_socket {
    uint8_t       protocol;  // e.g. IPPROTO_ICMP (1)
    uint32_t      bound_addr; // 0 = any (host byte order)
    ring_buffer*  rx_buf;    // incoming packets queued here
    sync::spinlock lock;
};

/**
 * Create an AF_INET SOCK_DGRAM IPPROTO_ICMP socket.
 * Returns a resource_object with the inet socket ops.
 */
int32_t create_inet_icmp_socket(resource::resource_object** out);

/**
 * Register an ICMP socket to receive incoming packets.
 * Called during socket creation.
 */
void register_icmp_socket(inet_socket* sock);

/**
 * Unregister an ICMP socket. Called during socket close.
 */
void unregister_icmp_socket(inet_socket* sock);

/**
 * Deliver an ICMP packet to all registered ICMP sockets.
 * Called by icmp_recv() when a packet arrives that should be
 * delivered to userland (e.g. echo replies).
 * @param src_ip  Source IP in host byte order.
 * @param data    ICMP packet data (starting with icmp_header).
 * @param len     Length of ICMP packet.
 */
void deliver_to_icmp_sockets(uint32_t src_ip, const uint8_t* data, size_t len);

} // namespace net

#endif // STELLUX_NET_INET_SOCKET_H
