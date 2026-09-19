#ifndef STELLUX_NET_TCP_REASSEMBLY_H
#define STELLUX_NET_TCP_REASSEMBLY_H

#include "net/tcp/conn.h"
#include "net/tcp/wire.h"
#include "net/packet.h"

namespace net {
namespace tcp {

/**
 * What became of a segment's text: bytes now in the receive queue, whether
 * the packet was kept for later, and the FIN sequence the queue completed,
 * zero when none.
 */
struct payload_result {
    size_t   queued;
    bool     packet_taken;
    uint32_t fin_seq;
};

/**
 * @brief Receives a segment's text (RFC 9293 3.10.7.4 step 7): in-order bytes
 * join the receive queue and pull queued segments in behind them, bytes ahead
 * wait in the out-of-order queue, bytes already held are reported as a
 * duplicate for DSACK (RFC 2883). Caller holds the lock.
 */
payload_result take_payload_locked(tcp_conn* conn, packet* pkt, const tcp_header* hdr, uint32_t seq, size_t payload_len);

/**
 * @brief Records bytes the peer sent again, for the next acknowledgment to
 * report (RFC 2883 4). Caller holds the lock.
 */
void note_dsack_locked(tcp_conn* conn, uint32_t start, uint32_t end);

/**
 * @brief Frees every out-of-order segment. Caller holds the lock.
 */
void clear_out_of_order_locked(tcp_conn* conn);

/**
 * @brief The blocks the next acknowledgment reports (RFC 2018 4, RFC 2883
 * 4): a pending DSACK first, then the most recent contiguous blocks held.
 * Caller holds the lock.
 */
void fill_sack_blocks_locked(const tcp_conn* conn, tcp_options* opts);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_REASSEMBLY_H
