#ifndef STELLUX_NET_TCP_INFO_H
#define STELLUX_NET_TCP_INFO_H

#include "net/tcp/record.h"
#include "net/tcp/listen.h"
#include "net/tcp/timewait.h"
#include "net/interface.h"

namespace net {
namespace tcp {

constexpr size_t MAX_INFO_RECORDS = MAX_LISTENERS + MAX_REQUESTS + MAX_CONNECTIONS + MAX_TIMEWAIT;

// Values of tcp_record::kind, timer_kind, and flags, mirrored by stlx/net.h.
// tcp_record::state carries tcp_state values, a listener reports listen.
constexpr uint8_t INFO_KIND_LISTENER   = 0;
constexpr uint8_t INFO_KIND_REQUEST    = 1;
constexpr uint8_t INFO_KIND_CONNECTION = 2;
constexpr uint8_t INFO_KIND_TIMEWAIT   = 3;

constexpr uint8_t INFO_TIMER_NONE     = 0;
constexpr uint8_t INFO_TIMER_RTO      = 1;
constexpr uint8_t INFO_TIMER_ORPHAN   = 2;
constexpr uint8_t INFO_TIMER_TIMEWAIT = 3;
constexpr uint8_t INFO_TIMER_PROBE    = 4;

constexpr uint8_t INFO_TIMESTAMPS   = 1u << 0;
constexpr uint8_t INFO_SACK         = 1u << 1;
constexpr uint8_t INFO_WINDOW_SCALE = 1u << 2;
constexpr uint8_t INFO_ORPHANED     = 1u << 3;
constexpr uint8_t INFO_FIN_SENT     = 1u << 4;
constexpr uint8_t INFO_FIN_RCVD     = 1u << 5;

/**
 * One table entry as userland sees it, mirrored by stlx/net.h. Addresses and
 * ports are in host byte order, queues and buffers are in bytes, and fields a
 * kind does not have are zero.
 */
struct tcp_record {
    uint8_t  kind;
    uint8_t  state;
    uint8_t  timer_kind;
    uint8_t  flags;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_addr;
    uint32_t remote_addr;
    char     iface[IFACE_NAME_MAX];
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t snd_wnd;
    uint32_t rcv_wnd;
    uint16_t snd_mss;
    uint8_t  snd_wscale;
    uint8_t  rcv_wscale;
    uint16_t backlog;
    uint16_t requests;
    uint16_t accepted;
    uint8_t  retransmits;
    uint8_t  backoff;
    uint32_t timer_ms;
    int32_t  error;
    uint32_t rcv_queued;
    uint32_t rcv_buf;
    uint32_t snd_queued;
    uint32_t snd_buf;
    uint32_t ooo_bytes;
    uint32_t cwnd;
    uint32_t srtt_us;
    uint32_t rttvar_us;
    uint32_t rto_ms;
    uint32_t total_retransmits;
    uint16_t ooo_packets;
    uint16_t unacked;
};
static_assert(sizeof(tcp_record) == 116);

/**
 * Stack-wide counts since boot and the current table occupancy, mirrored by
 * stlx/net.h.
 */
struct tcp_counters {
    uint64_t segments_in;
    uint64_t segments_out;
    uint64_t retransmits;
    uint64_t rsts_sent;
    uint64_t rsts_received;
    uint64_t checksum_failures;
    uint64_t listen_drops;
    uint64_t paws_drops;
    uint64_t challenge_acks;
    uint32_t listeners;
    uint32_t requests;
    uint32_t connections;
    uint32_t timewaits;
};
static_assert(sizeof(tcp_counters) == 88);

/**
 * The SIOCGTCPINFO argument, mirrored by stlx/net.h. Userland sets `capacity`
 * and `records`, the kernel writes `count` records there, the number that
 * existed in `total`, and the counters.
 */
struct tcp_info {
    uint32_t     capacity;
    uint32_t     count;
    uint32_t     total;
    uint32_t     reserved;
    uint64_t     records;
    tcp_counters counters;
};
static_assert(sizeof(tcp_info) == 112);

enum class counter : uint8_t {
    segments_in       = 0,
    segments_out      = 1,
    retransmits       = 2,
    rsts_sent         = 3,
    rsts_received     = 4,
    checksum_failures = 5,
    listen_drops      = 6,
    paws_drops        = 7,
    challenge_acks    = 8,
    count             = 9,
};

/**
 * @brief Adds one to a stack-wide counter. Safe from any context.
 */
void increment(counter which);

/**
 * @brief Writes the counters and the table occupancy into `out`.
 */
void describe_counters(tcp_counters* out);

/**
 * @brief Writes a listener into `out`.
 */
void describe_listener(tcp_listener* listener, tcp_record* out);

/**
 * @brief Writes a request, connection, or TIME_WAIT record into `out`, read
 * under the record's lock.
 */
void describe_record(record* rec, tcp_record* out);

/**
 * @brief Answers SIOCGTCPINFO for the tcp_info at `user_info`: listeners
 * first, then the table's records, as many as `capacity` allows.
 * @return OK, ERR_INVALID for an unreadable or unwritable argument,
 *         ERR_NO_MEMORY when the record list cannot be allocated.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t query_info(uint64_t user_info);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_INFO_H
