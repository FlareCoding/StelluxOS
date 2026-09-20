#ifndef STELLUX_NET_TCP_LISTEN_H
#define STELLUX_NET_TCP_LISTEN_H

#include "net/tcp/conn.h"
#include "net/tcp/wire.h"
#include "net/packet.h"

namespace net {
namespace tcp {

constexpr size_t   MAX_LISTENERS  = 64;
constexpr size_t   MAX_REQUESTS   = 512;
constexpr uint16_t MAX_BACKLOG    = 128;
constexpr uint8_t  SYNACK_RETRIES = 5;

struct tcp_listener;

/**
 * A local endpoint as bind names it: an address or the wildcard, a port, an
 * optional interface pin, and whether it may share its port.
 */
struct endpoint {
    ipv4::ipv4_addr addr;
    uint16_t        port;
    interface*      iface;
    bool            reuseaddr;
};

/**
 * A SYN received on a listener, awaiting the ACK that completes the handshake.
 * It holds everything the connection it becomes needs and nothing more, so a
 * flood of SYNs costs request records, not connections.
 */
struct tcp_request : record {
    uint32_t iss;
    uint32_t irs;
    uint32_t ts_recent;
    uint32_t ts_offset;
    uint16_t peer_mss;
    uint8_t  snd_wscale;
    uint8_t  rcv_wscale;
    bool     wscale_ok;
    bool     sack_ok;
    bool     ts_ok;
    bool     ecn_ok;

    timer::deadline_timer timer;
    bool                  timer_armed;
    uint64_t              timer_deadline_ns;
    uint8_t               retransmits;

    tcp_listener* listener;
};

/**
 * A listening endpoint. Requests are counted against `backlog` and completed
 * connections wait in `accept_queue` until accept takes them. Held by
 * reference from its socket and from every request that points back to it.
 */
struct tcp_listener : rc::ref_counted<tcp_listener> {
    endpoint     local;
    conn_options options;
    bool         closed;

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
 * @brief True when a segment could match both endpoints: the same port, not
 * pinned to different interfaces, and the same address or one wildcard without
 * `reuseaddr` on both.
 */
bool endpoints_conflict(const endpoint& a, const endpoint& b);

/**
 * @brief True when a listener conflicts with `local`.
 */
bool listener_conflicts(const endpoint& local);

/**
 * @brief Allocates a listener on `local`, held by the one reference returned.
 * @return The listener, or nullptr when memory is exhausted.
 */
tcp_listener* alloc_listener(const endpoint& local);

/**
 * @brief Adds `listener` to the listener table, the table taking a reference of
 * its own.
 * @return OK, ERR_IN_USE when an existing listener conflicts, ERR_FULL when the
 *         table is full.
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

/**
 * @brief Fills `out` with up to `max` listeners, each with a reference the
 * caller drops, and reports in `total` how many exist.
 * @return The number of listeners written.
 */
size_t collect_listeners(tcp_listener** out, size_t max, size_t* total);

/**
 * @brief Removes `listener` from the table with every request it still holds,
 * dropping the table's references.
 */
void listener_close(tcp_listener* listener);

/**
 * @brief Consumes a SYN for `listener`: a request record answers it with a
 * SYN-ACK and waits for the completing ACK, or the SYN is dropped when the
 * backlog or the request capacity is full (RFC 9293 3.10.7.2).
 */
int32_t listen_input(tcp_listener* listener, packet* pkt, const tcp_header* hdr, const tcp_options& opts);

/**
 * @brief Consumes a segment for `request`. A retransmitted SYN is answered with
 * the SYN-ACK again, the ACK completing the handshake turns the request into a
 * connection queued for accept, a wrong acknowledgment is reset, and a reset
 * from the peer returns the request to LISTEN (RFC 9293 3.10.7.4).
 */
int32_t request_input(tcp_request* request, packet* pkt, const tcp_header* hdr, const tcp_options& opts);

/**
 * @brief Takes the oldest connection waiting on `listener`, with the reference
 * the queue held.
 * @return The connection, or an empty reference when none is waiting.
 */
rc::strong_ref<tcp_conn> pop_accepted(tcp_listener* listener);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_LISTEN_H
