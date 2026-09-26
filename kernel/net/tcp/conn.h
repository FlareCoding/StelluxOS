#ifndef STELLUX_NET_TCP_CONN_H
#define STELLUX_NET_TCP_CONN_H

#include "net/tcp/record.h"
#include "net/tcp/byte_queue.h"
#include "net/tcp/sent_segment.h"
#include "net/tcp/congestion.h"
#include "net/tcp/wire.h"
#include "net/packet.h"
#include "common/list.h"
#include "rc/strong_ref.h"
#include "sync/wait_queue.h"
#include "timer/timer.h"

namespace resource { struct resource_object; }

namespace net {
namespace tcp {

struct tcp_listener;

constexpr size_t   MAX_CONNECTIONS    = 256;
constexpr size_t   TABLE_BUCKETS      = 1024;
constexpr uint64_t TIMEOUT_INIT_NS    = 1000000000ULL; // RFC 6298 2.1, before any RTT sample
constexpr uint8_t  SYN_RETRIES        = 6;
constexpr uint8_t  ORPHAN_RETRIES     = 8;
constexpr uint16_t EPHEMERAL_PORT_MIN = 49152;
constexpr uint16_t EPHEMERAL_PORT_MAX = 65535;
constexpr uint64_t ISN_TICK_NS        = 4000;    // RFC 6528 3, the clock behind M
constexpr uint64_t TIMESTAMP_TICK_NS  = 1000000; // RFC 7323 5.4 allows 1 ms to 1 s
constexpr uint64_t TIMEOUT_MAX_NS     = 120000000000ULL;
constexpr uint64_t FIN_TIMEOUT_NS     = 60000000000ULL;
constexpr uint16_t DEFAULT_MSS        = 536;     // RFC 9293 3.7.1, when the peer sends none
constexpr uint16_t MIN_MSS            = 88;
constexpr uint64_t TS_RECENT_MAX_AGE_NS = 24ULL * 24 * 3600 * 1000000000ULL; // RFC 7323 5.5
constexpr size_t   RCV_CHUNKS_INITIAL = MIN_BUF / CHUNK_PAYLOAD;
constexpr uint32_t RCV_WND_INITIAL    = RCV_CHUNKS_INITIAL * CHUNK_PAYLOAD; // what an empty queue can take
constexpr uint64_t DELACK_NS          = 40000000ULL; // RFC 1122 4.2.3.2 allows up to 500 ms
constexpr uint8_t  MAX_QUICKACKS      = 16;
constexpr size_t   SND_CHUNKS_INITIAL = MIN_BUF / CHUNK_PAYLOAD;
constexpr size_t   SENT_SEGMENT_MARGIN = 8;
constexpr uint8_t  DELIVERY_PROBLEM_RETRIES = 3; // RFC 1122 4.2.3.5 R1
constexpr uint8_t  DATA_RETRIES       = 15;      // RFC 1122 4.2.3.5 R2
constexpr size_t   MAX_BURST          = 16;
constexpr size_t   MAX_OOO_PACKETS    = 64;
constexpr uint16_t KEEPALIVE_IDLE_S     = 7200; // RFC 9293 3.8.4, no less than two hours
constexpr uint16_t KEEPALIVE_INTERVAL_S = 75;
constexpr uint8_t  KEEPALIVE_PROBES     = 9;
constexpr int32_t  MAX_KEEPALIVE_S      = 32767;
constexpr int32_t  MAX_KEEPALIVE_PROBES = 127;

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
    none   = 0,
    rto    = 1,
    orphan = 2,
    probe  = 3,
};

/**
 * The choices a socket makes ahead of any connection, handed to each one it
 * opens or accepts.
 */
struct conn_options {
    bool     nodelay;
    uint16_t snd_mss_cap; // Zero for none
    bool     keepalive;
    uint16_t keepalive_idle_s     = KEEPALIVE_IDLE_S;
    uint16_t keepalive_interval_s = KEEPALIVE_INTERVAL_S;
    uint8_t  keepalive_probes     = KEEPALIVE_PROBES;
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
    bool      rcv_shutdown;

    // Send sequence space (RFC 9293 3.3.1)
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint32_t snd_wl1;
    uint32_t snd_wl2;
    uint32_t iss;
    uint32_t max_snd_wnd; // Largest window the peer ever advertised (RFC 5961 5.2)
    uint16_t snd_mss;
    uint16_t snd_mss_cap;
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

    // Retransmits what snd_una waits for or probes a closed window. Kind none
    // means disarmed, and a callback acts only once this deadline has passed
    timer::deadline_timer send_timer;
    timer_kind            send_timer_kind;
    uint64_t              send_timer_deadline_ns;
    uint8_t               retransmits;
    uint8_t               backoff;
    uint8_t               unanswered_probes;
    uint32_t              total_retransmits;
    uint64_t              peer_acked_ns;

    // Round-trip estimate (RFC 6298), srtt_us zero until the first sample
    uint32_t srtt_us;
    uint32_t rttvar_us;
    uint64_t rto_ns;

    // Acknowledgment strategy (RFC 5681 4.2): an owed ACK waits on the ack
    // timer unless a rule below sends it at once
    timer::deadline_timer ack_timer;
    bool                  ack_timer_armed;
    uint64_t              ack_timer_deadline_ns;

    // Keepalive (RFC 9293 3.8.4), off unless the socket asked
    timer::deadline_timer keepalive_timer;
    bool                  keepalive_armed;
    uint64_t              keepalive_deadline_ns;
    bool                  keepalive;
    uint16_t              keepalive_idle_s;
    uint16_t              keepalive_interval_s;
    uint8_t               keepalive_probes;
    bool                  ack_pending;
    uint8_t               quick_acks;   // Immediate ACKs left before delaying begins
    uint32_t              rcv_acked;    // rcv_nxt as of the last ACK sent
    uint64_t              rcv_last_ns;  // When payload last arrived

    byte_queue       rcv_queue;
    sync::wait_queue rx_wq; // readers

    list::head<packet, &packet::link> ooo_queue; // Segments ahead of rcv_nxt, by sequence
    size_t                            ooo_bytes;
    sack_block                        recent_sacks[MAX_SACK_BLOCKS]; // Newest first
    uint8_t                           recent_sack_count;
    sack_block                        dsack;
    bool                              dsack_pending;

    byte_queue       snd_queue;
    sent_segments    sent;
    sync::wait_queue tx_wq; // writers
    uint32_t         snd_sml; // End of the last partial segment sent
    bool             nodelay;
    bool             fin_pending;

    // Congestion control (RFC 5681), windows in bytes
    uint32_t              cwnd;
    uint32_t              ssthresh;
    uint32_t              prior_cwnd;
    uint32_t              prior_ssthresh;
    uint32_t              high_seq;    // RFC 6582 recover
    uint32_t              bytes_acked; // RFC 3465, toward the next increase
    uint32_t              max_in_flight;
    uint32_t              in_flight_window_end;
    uint64_t              last_data_sent_ns;
    bool                  cwnd_limited;
    uint8_t               dupacks;
    uint8_t               frto_step;   // RFC 5682 2.2, zero when not in use
    recovery_state        recovery;
    const congestion_ops* congestion;
    alignas(8) uint8_t    congestion_state[CONGESTION_STATE_SIZE];

    resource::resource_object* owner;
    sync::wait_queue           conn_wq; // connect and close waiters
    int32_t                    pending_error;
    int32_t                    soft_error;
    list::node                 accept_link;
};

/**
 * @brief Prepares the connection table and draws the secret that hashes tuples.
 */
int32_t init_tables();

/**
 * @brief Draws the secrets behind initial sequence numbers and timestamp offsets.
 */
int32_t init_sequence_numbers();

/**
 * @brief The clock every TCP deadline and timestamp is taken from.
 */
uint64_t now_ns();

/**
 * @brief For tests only: reads the clock through `fn`, or the real clock when null.
 */
void __dbg_test_set_clock(uint64_t (*fn)());

/**
 * @brief The timestamp a connection with `offset` sends now (RFC 7323 5.4).
 */
uint32_t timestamp_value(uint32_t offset);

/**
 * @brief The window shift this host advertises, the smallest that lets the
 * largest receive buffer fit a sixteen-bit window (RFC 7323 2.2).
 */
uint8_t receive_window_scale();

/**
 * @brief Sets `rcv_wnd` to what the receive queue can still take, rounded to
 * the window scale and never moving the advertised right edge left (RFC 9293
 * 3.8.6.2.2). Caller holds the lock.
 */
void update_receive_window_locked(tcp_conn* conn);

/**
 * @brief Records that a segment acknowledging `rcv_nxt` with the current
 * window is going out, so nothing owed remains. Caller holds the lock.
 */
void mark_ack_sent_locked(tcp_conn* conn);

/**
 * @brief Sets the initial congestion window (RFC 6928) and the record cap
 * from `snd_mss`. Caller holds the lock.
 */
void configure_send_path_locked(tcp_conn* conn);

/**
 * @brief Queued bytes not yet sent. An unacknowledged SYN or FIN holds a
 * sequence number, not a byte. Caller holds the lock.
 */
inline size_t unsent_bytes(const tcp_conn* conn) {
    uint32_t in_flight = conn->snd_nxt - conn->snd_una;
    uint32_t control = (conn->snd_una == conn->iss ? 1u : 0u) +
                       (conn->fin_sent && conn->snd_una != conn->snd_nxt ? 1u : 0u);
    in_flight = in_flight > control ? in_flight - control : 0;

    return conn->snd_queue.size() > in_flight ? conn->snd_queue.size() - in_flight : 0;
}

inline bool is_synchronized(tcp_state state) {
    return state == tcp_state::established || state == tcp_state::fin_wait_1 ||
           state == tcp_state::fin_wait_2 || state == tcp_state::close_wait ||
           state == tcp_state::closing || state == tcp_state::last_ack;
}

/**
 * @brief The initial sequence number for a new connection on `key` (RFC 6528):
 * a clock advancing every ISN_TICK_NS plus a keyed hash of the tuple, so the
 * numbers of one tuple advance in time while another host cannot predict them.
 */
uint32_t initial_sequence(const tuple& key);

/**
 * @brief The offset added to every timestamp a connection on `key` sends, fixed
 * for the tuple and unpredictable to others (RFC 7323 7), so connections cannot
 * be correlated through the timestamp clock.
 */
uint32_t timestamp_offset(const tuple& key);

/**
 * @brief Allocates a connection for `key` on `iface` in the closed state, held
 * by the one reference returned.
 * @return The connection, or nullptr when memory is exhausted.
 */
tcp_conn* alloc_conn(const tuple& key, interface* iface);

/**
 * @brief Gives a connection the choices its socket or listener made.
 */
inline void apply_options(tcp_conn* conn, const conn_options& options) {
    conn->nodelay = options.nodelay;
    conn->snd_mss_cap = options.snd_mss_cap;
    conn->keepalive = options.keepalive;
    conn->keepalive_idle_s = options.keepalive_idle_s;
    conn->keepalive_interval_s = options.keepalive_interval_s;
    conn->keepalive_probes = options.keepalive_probes;
}

/**
 * @brief Begins an active open (RFC 9293 3.10.1): a connection for `key` on
 * `iface` with `options` enters SYN_SENT, joins the table, sends its SYN, and
 * arms the retransmission timer.
 * @return OK with `out` holding the connection, ERR_IN_USE when the key is
 *         taken, ERR_FULL at capacity, ERR_NO_MEMORY.
 */
int32_t open_active(const tuple& key, interface* iface, rc::strong_ref<tcp_conn>* out,
                    const conn_options& options = {});

/**
 * @brief Ends the connection at once: a reset goes to the peer, the record
 * leaves the table, its timer is disarmed, and every waiter on it is woken
 * (RFC 9293 3.10 ABORT).
 */
void abort_connection(tcp_conn* conn);

/**
 * @brief Ends a handshake the peer or the network refused, without a reset:
 * `error` is what the socket will report, the record leaves the table, its
 * timer is disarmed, and every waiter on it is woken. A connection that has
 * completed its handshake in the meantime is left alone.
 */
void fail_connection(tcp_conn* conn, int32_t error);

/**
 * @brief Finishes a connection whose state was set to closed under its lock:
 * disarms its timer, removes it from the table, and wakes every waiter.
 */
void retire_connection(tcp_conn* conn);

/**
 * @brief Queues the FIN behind the pending data (RFC 9293 3.10.4) and wakes
 * writers, who find EPIPE. Nothing more happens once a FIN is queued.
 */
void shutdown_send(tcp_conn* conn);

/**
 * @brief Ends the receiving side without sending anything: readers wake and
 * read end-of-file once the queue is out.
 */
void shutdown_receive(tcp_conn* conn);

/**
 * @brief Lets the socket go: a reset when received data would be lost (RFC
 * 1122 4.2.2.13), otherwise the orderly close, continued as an orphan.
 */
void close_connection(tcp_conn* conn);

/**
 * @brief Finds the record for `key`, holding a reference for the caller.
 * @return The record, or an empty reference when nothing matches.
 */
rc::strong_ref<record> lookup(const tuple& key);

/**
 * @brief Inserts `rec` under its key, the table taking a reference of its own.
 * @return OK, ERR_IN_USE when the key is taken, ERR_FULL when its kind is at capacity.
 */
int32_t insert(record* rec);

/**
 * @brief Puts `fresh` in the table in place of `old`, whose key it must share,
 * moving the table's reference from one to the other.
 * @return OK, ERR_NOT_FOUND when `old` is not in the table, ERR_FULL when the
 *         kind of `fresh` is at capacity.
 */
int32_t replace(record* old, record* fresh);

/**
 * @brief Removes `rec` from the table and drops the table's reference.
 * @return OK, or ERR_NOT_FOUND when it was not in the table.
 */
int32_t remove(record* rec);

/**
 * @brief Records of `kind` in the table.
 */
size_t record_count(record_kind kind);

/**
 * @brief Fills `out` with up to `max` records of every kind, each with a
 * reference the caller drops, and reports in `total` how many the table held.
 * @return The number of records written.
 */
size_t collect_records(record** out, size_t max, size_t* total);

/**
 * @brief Takes one request of `listener` out of the table, handing the table's
 * reference to the caller.
 * @return The request, or an empty reference when none is left.
 */
rc::strong_ref<record> remove_one_request_of(const tcp_listener* listener);

/**
 * @brief True when any record, listener, or bound socket uses `port` as its
 * local port.
 */
bool is_local_port_taken(uint16_t port);

/**
 * @brief Hands out a port no endpoint uses, one lap on from the last one given.
 * @return OK, or ERR_FULL when the whole ephemeral range is in use.
 */
int32_t take_ephemeral_port(uint16_t* out);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_CONN_H
