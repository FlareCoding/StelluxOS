#ifndef STELLUX_NET_TCP_RECOVERY_H
#define STELLUX_NET_TCP_RECOVERY_H

#include "net/tcp/conn.h"

namespace net {
namespace tcp {

constexpr uint8_t  DUPACK_THRESHOLD          = 3; // RFC 5681 3.2
constexpr uint32_t LIMITED_TRANSMIT_SEGMENTS = 2; // RFC 3042

/**
 * @brief True for an acknowledgment that repeats the last one while data is
 * outstanding: no payload, no SYN or FIN, `ack` at SND.UNA, `window`
 * unchanged (RFC 5681 2). Caller holds the lock.
 */
bool is_duplicate_ack_locked(const tcp_conn* conn, uint8_t flags, size_t payload_len, uint32_t ack,
                             uint32_t window);

/**
 * @brief Counts a duplicate acknowledgment; the first one moves an open
 * connection into disorder. Caller holds the lock.
 */
void take_duplicate_ack_locked(tcp_conn* conn);

/**
 * @brief Clears the duplicate count once an acknowledgment moves SND.UNA,
 * returning a disordered connection to open. Caller holds the lock.
 */
void leave_disorder_locked(tcp_conn* conn);

/**
 * @brief Bytes allowed in flight beyond `cwnd` while in disorder, one
 * segment per duplicate acknowledgment up to two (RFC 3042).
 */
uint32_t limited_transmit_bytes(const tcp_conn* conn);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_RECOVERY_H
