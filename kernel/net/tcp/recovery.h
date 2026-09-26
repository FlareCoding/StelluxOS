#ifndef STELLUX_NET_TCP_RECOVERY_H
#define STELLUX_NET_TCP_RECOVERY_H

#include "net/tcp/conn.h"

namespace net {
namespace tcp {

constexpr uint8_t  DUPACK_THRESHOLD          = 3; // RFC 5681 3.2
constexpr uint32_t LIMITED_TRANSMIT_SEGMENTS = 2; // RFC 3042

enum class recovery_action : uint8_t {
    none       = 0,
    retransmit = 1,
    recovered  = 2,
};

/**
 * @brief True for an acknowledgment that repeats the last one while data is
 * outstanding: no payload, no SYN or FIN, `ack` at SND.UNA, `window`
 * unchanged (RFC 5681 2). Caller holds the lock.
 */
bool is_duplicate_ack_locked(const tcp_conn* conn, uint8_t flags, size_t payload_len, uint32_t ack,
                             uint32_t window);

/**
 * @brief Counts a duplicate acknowledgment: the first moves an open
 * connection into disorder, the third into recovery with the window halved
 * and the oldest segment to retransmit, later ones inflate the window by a
 * segment (RFC 5681 3.2, RFC 6582 3.2). Caller holds the lock.
 */
recovery_action take_duplicate_ack_locked(tcp_conn* conn);

/**
 * @brief Takes an acknowledgment that moved SND.UNA to `ack` covering
 * `acked_bytes`: disorder ends; in recovery a partial one deflates the window
 * and retransmits the next hole, a full one ends recovery (RFC 6582 3.2).
 * Caller holds the lock.
 */
recovery_action take_advancing_ack_locked(tcp_conn* conn, uint32_t ack, uint32_t acked_bytes);

/**
 * @brief Bytes allowed in flight beyond `cwnd` while in disorder, one
 * segment per duplicate acknowledgment up to two (RFC 3042).
 */
uint32_t limited_transmit_bytes(const tcp_conn* conn);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_RECOVERY_H
