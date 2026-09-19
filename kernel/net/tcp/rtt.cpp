#include "net/tcp/rtt.h"

namespace net {
namespace tcp {

static uint32_t absolute_difference(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

void take_rtt_sample_locked(tcp_conn* conn, uint64_t rtt_ns) {
    uint32_t sample_us = static_cast<uint32_t>(rtt_ns / 1000);
    if (sample_us == 0) {
        sample_us = 1;
    }

    if (conn->srtt_us == 0) {
        conn->srtt_us = sample_us;
        conn->rttvar_us = sample_us / 2;
    } else {
        conn->rttvar_us = conn->rttvar_us - conn->rttvar_us / 4 + absolute_difference(conn->srtt_us, sample_us) / 4;
        conn->srtt_us = conn->srtt_us - conn->srtt_us / 8 + sample_us / 8;
    }

    uint32_t spread_us = 4 * conn->rttvar_us > RTT_GRANULARITY_US ? 4 * conn->rttvar_us : RTT_GRANULARITY_US;
    uint64_t rto_ns = (static_cast<uint64_t>(conn->srtt_us) + spread_us) * 1000;
    if (rto_ns < RTO_MIN_NS) {
        rto_ns = RTO_MIN_NS;
    } else if (rto_ns > TIMEOUT_MAX_NS) {
        rto_ns = TIMEOUT_MAX_NS;
    }

    conn->rto_ns = rto_ns;
}

uint64_t rtt_from_echo_locked(const tcp_conn* conn, uint32_t ts_ecr) {
    if (!conn->ts_ok || ts_ecr == 0) {
        return 0;
    }

    uint32_t elapsed_ticks = timestamp_value(conn->ts_offset) - ts_ecr;
    uint64_t rtt_ns = static_cast<uint64_t>(elapsed_ticks) * TIMESTAMP_TICK_NS;

    return rtt_ns <= TIMEOUT_MAX_NS ? rtt_ns : 0;
}

} // namespace tcp
} // namespace net
