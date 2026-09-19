#ifndef STELLUX_NET_TCP_TIMEWAIT_H
#define STELLUX_NET_TCP_TIMEWAIT_H

#include "net/tcp/record.h"
#include "net/tcp/conn.h"
#include "net/tcp/wire.h"
#include "net/packet.h"
#include "timer/timer.h"

namespace net {
namespace tcp {

constexpr size_t   MAX_TIMEWAIT    = 1024;
constexpr uint64_t TIMEWAIT_LEN_NS = 60000000000ULL; // 2 MSL
constexpr int32_t  TIMEWAIT_REOPEN = 1;

/**
 * What remains of a connection in TIME_WAIT: enough to acknowledge a
 * retransmitted FIN and to tell a new connection on the same tuple from an
 * old duplicate (RFC 1122 4.2.2.13, RFC 6191). It replaces the connection in
 * the table so the connection's queues are freed at once.
 */
struct tcp_timewait : record {
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t ts_recent;
    uint64_t ts_recent_age_ns;
    bool     ts_ok;

    timer::deadline_timer timer;
    bool                  timer_armed;
    uint64_t              timer_deadline_ns;
};

/**
 * @brief Retires a connection that has finished its close and puts a record
 * for 2 MSL in its place. Without memory for the record it is retired alone.
 */
void enter_time_wait(tcp_conn* conn);

/**
 * @brief Consumes a segment for a TIME_WAIT record as RFC 9293 3.10.7.4,
 * RFC 1337, and RFC 1122 4.2.2.13 prescribe.
 * @return TIMEWAIT_REOPEN, with the packet left to the caller, when a newer
 *         SYN removed the record for a new connection, otherwise OK.
 */
int32_t timewait_input(tcp_timewait* tw, packet* pkt, const tcp_header* hdr, const tcp_options& opts);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_TIMEWAIT_H
