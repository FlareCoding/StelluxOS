#ifndef STELLUX_NET_TCP_RTT_H
#define STELLUX_NET_TCP_RTT_H

#include "net/tcp/conn.h"

namespace net {
namespace tcp {

constexpr uint64_t RTO_MIN_NS         = 200000000ULL; // RFC 6298 2.4 permits less than 1 s
constexpr uint32_t RTT_GRANULARITY_US = 1000;         // RFC 6298 2.3

/**
 * @brief Feeds one round trip into the estimator (RFC 6298 2.2 and 2.3) and
 * sets `rto_ns` within [RTO_MIN_NS, TIMEOUT_MAX_NS]. Caller holds the lock.
 */
void take_rtt_sample_locked(tcp_conn* conn, uint64_t rtt_ns);

/**
 * @brief The round trip a timestamp echo measures (RFC 7323 4.1), zero when
 * `ts_ecr` is not a timestamp this connection sent within the largest
 * timeout. Caller holds the lock.
 */
uint64_t rtt_from_echo_locked(const tcp_conn* conn, uint32_t ts_ecr);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_RTT_H
