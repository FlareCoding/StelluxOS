#ifndef STELLUX_NET_TCP_OUTPUT_H
#define STELLUX_NET_TCP_OUTPUT_H

#include "net/tcp/record.h"
#include "net/tcp/wire.h"

namespace net {
namespace tcp {

/**
 * @brief Sends a segment without payload for `key` through `iface`, from the
 * local address of the key to its remote one, carrying `flags`, `seq`, `ack`,
 * an unscaled `window`, and the options `opts` describes.
 * @return OK, or the error the route or the IPv4 layer reported.
 */
int32_t send_segment(interface* iface, const tuple& key, uint8_t flags, uint32_t seq, uint32_t ack,
                     uint16_t window, const tcp_options& opts);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_OUTPUT_H
