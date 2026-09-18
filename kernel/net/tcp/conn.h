#ifndef STELLUX_NET_TCP_CONN_H
#define STELLUX_NET_TCP_CONN_H

#include "net/tcp/record.h"
#include "common/list.h"
#include "sync/wait_queue.h"
#include "timer/timer.h"

namespace resource { struct resource_object; }

namespace net {
namespace tcp {

constexpr size_t   MAX_CONNECTIONS  = 256;
constexpr uint64_t TIMEOUT_INIT_NS  = 1000000000ULL; // RFC 6298 2.1, before any RTT sample
constexpr uint8_t  SYN_RETRIES      = 6;

/**
 * Connection states of RFC 9293 3.3.2. `listen` belongs to a listener and
 * `time_wait` to a TIME-WAIT record, never to a connection; both are here so
 * every record can report its state in one vocabulary.
 */
enum class tcp_state : uint8_t {
    closed      = 0,
    listen      = 1,
    syn_sent    = 2,
    syn_rcvd    = 3,
    established = 4,
    fin_wait_1  = 5,
    fin_wait_2  = 6,
    close_wait  = 7,
    closing     = 8,
    last_ack    = 9,
    time_wait   = 10,
};

enum class timer_kind : uint8_t {
    none = 0,
    rto  = 1,
};

/**
 * One connection, the RFC's TCB, grouped the way RFC 9293 groups it. Sequence
 * variables are host order. Everything is guarded by the record lock except
 * the reference count and the wait queues.
 */
struct tcp_conn : record {
    tcp_state state;
    bool      passive;
    bool      sack_ok;
    bool      ts_ok;
    bool      wscale_ok;
    bool      ecn_ok;
    bool      fin_sent;
    bool      fin_rcvd;
    bool      orphaned;

    // Send sequence space (RFC 9293 3.3.1)
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint32_t snd_wl1;
    uint32_t snd_wl2;
    uint32_t iss;
    uint32_t max_snd_wnd; // Largest window the peer ever advertised (RFC 5961 5.2)
    uint16_t snd_mss;
    uint8_t  snd_wscale;

    // Receive sequence space
    uint32_t rcv_nxt;
    uint32_t rcv_wnd;
    uint32_t irs;
    uint32_t rcv_adv;     // Right edge last advertised, never moved left
    uint16_t rcv_mss;
    uint8_t  rcv_wscale;

    // Timestamps (RFC 7323)
    uint32_t ts_recent;
    uint64_t ts_recent_age_ns;
    uint32_t ts_offset;

    // The one timer the handshake and the close use, retransmitting what
    // snd_una still waits for. `timer_generation` moves on every start and stop
    // so a callback that lost a race does nothing.
    timer::deadline_timer send_timer;
    timer_kind            send_timer_kind;
    uint32_t              timer_generation;
    uint8_t               retransmits;

    resource::resource_object* owner;
    sync::wait_queue           conn_wq; // connect and close waiters
    int32_t                    pending_error;
    list::node                 accept_link;
};

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_CONN_H
