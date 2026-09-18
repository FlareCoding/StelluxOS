#ifndef STELLUX_NET_TCP_TIMEWAIT_H
#define STELLUX_NET_TCP_TIMEWAIT_H

#include "net/tcp/record.h"
#include "timer/timer.h"

namespace net {
namespace tcp {

constexpr size_t   MAX_TIMEWAIT    = 1024;
constexpr uint64_t TIMEWAIT_LEN_NS = 60000000000ULL; // 2 MSL

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
    uint32_t              timer_generation;
};

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_TIMEWAIT_H
