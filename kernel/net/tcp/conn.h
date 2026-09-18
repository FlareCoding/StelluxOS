#ifndef STELLUX_NET_TCP_CONN_H
#define STELLUX_NET_TCP_CONN_H

#include "net/tcp/record.h"
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
constexpr uint16_t EPHEMERAL_PORT_MIN = 49152;
constexpr uint16_t EPHEMERAL_PORT_MAX = 65535;
constexpr uint64_t ISN_TICK_NS        = 4000;    // RFC 6528 3, the clock behind M
constexpr uint64_t TIMESTAMP_TICK_NS  = 1000000; // RFC 7323 5.4 allows 1 ms to 1 s
constexpr uint64_t TIMEOUT_MAX_NS     = 120000000000ULL;
constexpr uint16_t DEFAULT_MSS        = 536;     // RFC 9293 3.7.1, when the peer sends none
constexpr size_t   RCV_BUF_INITIAL    = 16 * 1024;
constexpr size_t   RCV_BUF_MAX        = 1024 * 1024;

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
    // snd_una still waits for. The kind is none while disarmed, and a callback
    // fires only once the record's own deadline has passed, so one that lost a
    // race to a cancel or a re-arm does nothing.
    timer::deadline_timer send_timer;
    timer_kind            send_timer_kind;
    uint64_t              send_timer_deadline_ns;
    uint8_t               retransmits;

    resource::resource_object* owner;
    sync::wait_queue           conn_wq; // connect and close waiters
    int32_t                    pending_error;
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
