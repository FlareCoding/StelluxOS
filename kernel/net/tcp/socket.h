#ifndef STELLUX_NET_TCP_SOCKET_H
#define STELLUX_NET_TCP_SOCKET_H

#include "net/tcp/listen.h"
#include "resource/resource.h"

namespace net {
namespace tcp {

constexpr size_t MAX_SOCKETS             = MAX_CONNECTIONS + MAX_LISTENERS;
constexpr size_t EPHEMERAL_BIND_ATTEMPTS = 8; // ports lost to a racing bind before giving up

/**
 * A stream socket as userland holds it, from creation through bind to the
 * listener or connection it comes to stand for, each held by reference.
 * The lock guards every field.
 */
struct tcp_socket {
    endpoint                     local;
    conn_options                 options;
    bool                         bound;
    bool                         connecting;
    bool                         linger;
    uint32_t                     linger_seconds;
    rc::strong_ref<tcp_listener> listener;
    rc::strong_ref<tcp_conn>     conn;
    sync::spinlock               lock;
};

/**
 * @brief Allocates an unbound socket and registers it.
 * @return The socket, or nullptr when the table is full or memory is exhausted.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE tcp_socket* socket_open();

/**
 * @brief Unregisters the socket, releasing what it stands for, and frees it.
 * With SO_LINGER the closing program waits for its FIN to be acknowledged.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void socket_close(tcp_socket* sock);

/**
 * @brief Blocks the calling task until the connection's FIN is acknowledged,
 * returning true, or until `timeout_ns` passes or a signal arrives.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool wait_fin_acknowledged(tcp_conn* conn, uint64_t timeout_ns);

/**
 * @brief Claims `addr` and `port` for the socket, an ephemeral port when `port`
 * is zero. The address must be the wildcard or one this host owns.
 * @return OK, ERR_INVALID when already bound, ERR_NOT_LOCAL for a foreign
 *         address, ERR_IN_USE when a socket or listener conflicts, ERR_FULL
 *         when no ephemeral port is free.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t socket_bind(tcp_socket* sock, const ipv4::ipv4_addr& addr, uint16_t port);

/**
 * @brief Turns the socket into a listener with `backlog` pending connections,
 * binding an ephemeral port first when it has none. On a socket already
 * listening only the backlog changes.
 * @return OK, ERR_INVALID when connected, ERR_IN_USE when a listener
 *         conflicts, ERR_FULL when no listener slot or ephemeral port is free.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t socket_listen(tcp_socket* sock, uint16_t backlog);

/**
 * @brief True when a bound socket uses `port`.
 */
bool is_socket_port(uint16_t port);

/**
 * @brief The resource operations of a stream socket.
 */
const resource::resource_ops* socket_ops();

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_SOCKET_H
