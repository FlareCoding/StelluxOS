#ifndef STELLUX_NET_UDP_SOCKET_H
#define STELLUX_NET_UDP_SOCKET_H

#include "common/types.h"
#include "net/packet.h"
#include "net/ipv4.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace resource { struct resource_ops; }

namespace net {
namespace udp {

constexpr size_t MAX_SOCKETS        = 32;
constexpr size_t SOCKET_QUEUE_DEPTH = 16; // datagrams held per socket, oldest dropped first

/**
 * One UDP socket. It receives datagrams sent to `local_port` on `local_addr`
 * or on every address when that is unspecified. They wait in `rx_queue` until
 * recvfrom takes them, waking a reader asleep on `rx_wq`.
 */
struct udp_socket {
    ipv4::ipv4_addr  local_addr;
    uint16_t         local_port; // Host order, zero until bound
    interface*       iface;
    sync::spinlock   lock;       // Guards rx_queue
    packet_list      rx_queue;
    sync::wait_queue rx_wq;      // Readers waiting for rx_queue to fill
};

/**
 * @brief Allocates an unbound socket and registers it.
 * @return The socket, or nullptr when the table is full or memory is exhausted.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE udp_socket* socket_open();

/**
 * @brief Unregisters the socket, releasing its port, and frees it with any
 * datagrams still waiting.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void socket_close(udp_socket* sock);

/**
 * @brief Claims `port` on `addr` for the socket or an ephemeral port when `port`
 * is zero.
 * @return OK, ERR_IN_USE when the port is taken, ERR_FULL when no ephemeral
 *         port is free, ERR_INVALID when the socket is already bound.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t socket_bind(udp_socket* sock, const ipv4::ipv4_addr& addr, uint16_t port);

/*
 * Delivers a valid datagram whose window starts at the UDP header to the socket
 * bound to its destination, waking a reader. ERR_NOT_FOUND leaves it with the caller.
 */
int32_t socket_deliver(packet* pkt);

/*
 * The resource operations a UDP socket's resource object dispatches through.
 */
const resource::resource_ops* socket_ops();

} // namespace udp
} // namespace net

#endif // STELLUX_NET_UDP_SOCKET_H
