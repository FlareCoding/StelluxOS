#ifndef STELLUX_NET_ICMP_SOCKET_H
#define STELLUX_NET_ICMP_SOCKET_H

#include "common/types.h"
#include "net/packet.h"
#include "sync/spinlock.h"

namespace resource { struct resource_ops; }

namespace net {
namespace icmp {

constexpr size_t MAX_SOCKETS        = 16;
constexpr size_t SOCKET_QUEUE_DEPTH = 8; // replies held per socket, oldest dropped first

/**
 * One ping socket. Requests sent through it carry `id`, and replies with that
 * identifier wait in `rx_queue` until recvfrom takes them. Lives in
 * unprivileged memory because the driver task enqueues replies while lowered.
 */
struct icmp_socket {
    uint16_t       id;
    sync::spinlock lock; // Guards rx_queue
    packet_list    rx_queue;
};

/*
 * Allocates a socket with an unused identifier and registers it.
 * Returns nullptr when the table is full or memory is exhausted.
 */
icmp_socket* socket_open();

/*
 * Unregisters the socket and frees it with any replies still waiting.
 */
void socket_close(icmp_socket* sock);

/*
 * Consumes an echo reply, queueing it on the socket that owns its identifier
 * or freeing it when no socket does.
 */
void socket_deliver(packet* pkt);

/*
 * The resource operations a ping socket's resource object dispatches through.
 */
const resource::resource_ops* socket_ops();

} // namespace icmp
} // namespace net

#endif // STELLUX_NET_ICMP_SOCKET_H
