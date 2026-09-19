#ifndef STELLUX_NET_TCP_INPUT_H
#define STELLUX_NET_TCP_INPUT_H

#include "net/tcp/conn.h"
#include "net/tcp/wire.h"
#include "net/packet.h"

namespace net {
namespace tcp {

constexpr uint32_t CHALLENGE_ACK_LIMIT     = 100; // per second across the host (RFC 5961 5.3)
constexpr uint64_t CHALLENGE_ACK_WINDOW_NS = 1000000000ULL;

/**
 * @brief Consumes a segment for `conn`, processing it as the connection's state
 * prescribes (RFC 9293 3.10.7).
 */
int32_t conn_input(tcp_conn* conn, packet* pkt, const tcp_header* hdr, const tcp_options& opts);

/**
 * @brief Takes one of the ACKs the host may send this second in answer to a
 * segment it did not accept, so a flood of such segments cannot use this host
 * to flood another or to count its answers (RFC 5961 5.3).
 * @return True when the ACK may be sent.
 */
bool take_challenge_ack();

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_INPUT_H
