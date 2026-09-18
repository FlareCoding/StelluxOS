#ifndef STELLUX_NET_TCP_INPUT_H
#define STELLUX_NET_TCP_INPUT_H

#include "net/tcp/conn.h"
#include "net/tcp/wire.h"
#include "net/packet.h"

namespace net {
namespace tcp {

/**
 * @brief Consumes a segment for `conn`, processing it as the connection's state
 * prescribes (RFC 9293 3.10.7).
 */
int32_t conn_input(tcp_conn* conn, packet* pkt, const tcp_header* hdr, const tcp_options& opts);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_INPUT_H
