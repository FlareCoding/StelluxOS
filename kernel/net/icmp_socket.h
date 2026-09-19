#ifndef STELLUX_NET_ICMP_SOCKET_H
#define STELLUX_NET_ICMP_SOCKET_H

#include "common/types.h"
#include "net/packet.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace resource { struct resource_ops; }

namespace net {
namespace icmp {

constexpr size_t MAX_SOCKETS        = 16;
constexpr size_t SOCKET_QUEUE_DEPTH = 8; // replies held per socket, oldest dropped first

/**
 * One ping socket. Requests sent through it carry `id`, and replies with that
 * identifier wait in `rx_queue` until recvfrom takes them, waking a reader
 * asleep on `rx_wq`. Protocol state, so it lives in unprivileged memory. The
 * lock is held with interrupts disabled on every path, which the sleep and
 * wake protocol requires.
 */
struct icmp_socket {
    uint16_t         id;
    sync::spinlock   lock; // Guards rx_queue
    packet_list      rx_queue;
    sync::wait_queue rx_wq; // Readers waiting for rx_queue to fill
};

/**
 * @brief Allocates a socket with an unused identifier and registers it.
 * @return The socket, or nullptr when the table is full or memory is exhausted.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE icmp_socket* socket_open();

/**
 * @brief Unregisters the socket and frees it with any replies still waiting.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void socket_close(icmp_socket* sock);

/*
 * Consumes an echo reply, queueing it on the socket owning its identifier and waking
 * a reader, or frees it when none does. Elevates internally, callable lowered.
 */
void socket_deliver(packet* pkt);

/*
 * The resource operations a ping socket's resource object dispatches through.
 */
const resource::resource_ops* socket_ops();

} // namespace icmp
} // namespace net

#endif // STELLUX_NET_ICMP_SOCKET_H
