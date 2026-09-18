#ifndef STELLUX_NET_TCP_LISTEN_H
#define STELLUX_NET_TCP_LISTEN_H

#include "net/tcp/conn.h"

namespace net {
namespace tcp {

constexpr size_t   MAX_LISTENERS  = 64;
constexpr size_t   MAX_REQUESTS   = 512;
constexpr uint16_t MAX_BACKLOG    = 128;
constexpr uint8_t  SYNACK_RETRIES = 5;

struct tcp_listener;

/**
 * A SYN received on a listener, awaiting the ACK that completes the handshake.
 * It holds everything the connection it becomes needs and nothing more, so a
 * flood of SYNs costs request records, not connections.
 */
struct tcp_request : record {
    uint32_t iss;
    uint32_t irs;
    uint32_t ts_recent;
    uint16_t peer_mss;
    uint8_t  snd_wscale;
    uint8_t  rcv_wscale;
    bool     sack_ok;
    bool     ts_ok;
    bool     ecn_ok;

    timer::deadline_timer timer;
    uint32_t              timer_generation;
    uint8_t               retransmits;

    tcp_listener* listener;
};

/**
 * A listening endpoint. Requests are counted against `backlog` and completed
 * connections wait in `accept_queue` until accept takes them. Held by
 * reference from its socket and from every request that points back to it.
 */
struct tcp_listener : rc::ref_counted<tcp_listener> {
    ipv4::ipv4_addr local_addr;
    uint16_t        local_port;
    interface*      iface;
    bool            reuseaddr;

    uint16_t backlog;
    uint16_t request_count;
    uint16_t accept_count;

    list::head<tcp_conn, &tcp_conn::accept_link> accept_queue;
    sync::wait_queue                             accept_wq;
    sync::spinlock                               lock;

    /**
     * @brief Frees the listener once the last reference is gone.
     */
    static void ref_destroy(tcp_listener* self);
};

/**
 * @brief Allocates a listener for `local_addr` and `local_port`, unspecified
 * for every address, held by the one reference returned.
 * @return The listener, or nullptr when memory is exhausted.
 */
tcp_listener* alloc_listener(const ipv4::ipv4_addr& local_addr, uint16_t local_port);

/**
 * @brief Adds `listener` to the listener table, the table taking a reference of
 * its own. A port is shared only between listeners pinned to different
 * interfaces, or between a wildcard and a specific address when both set
 * `reuseaddr`.
 * @return OK, ERR_IN_USE when the port is taken, ERR_FULL when the table is full.
 */
int32_t listener_insert(tcp_listener* listener);

/**
 * @brief Removes `listener` from the table and drops the table's reference.
 * @return OK, or ERR_NOT_FOUND when it was not in the table.
 */
int32_t listener_remove(tcp_listener* listener);

/**
 * @brief The listener for a SYN to `local_addr` and `port` arriving on `iface`,
 * an exact address before the wildcard, holding a reference for the caller.
 * @return The listener, or an empty reference when none listens there.
 */
rc::strong_ref<tcp_listener> listener_lookup(const ipv4::ipv4_addr& local_addr, uint16_t port,
                                             interface* iface);

/**
 * @brief True when a listener uses `port`.
 */
bool is_listener_port(uint16_t port);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_LISTEN_H
