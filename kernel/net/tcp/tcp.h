#ifndef STELLUX_NET_TCP_TCP_H
#define STELLUX_NET_TCP_TCP_H

#include "net/packet.h"

namespace net {
namespace tcp {

/**
 * @brief Consumes a segment whose window starts at the TCP header with the
 * interface set and the IPv4 header marked. A segment no connection claims is
 * answered with a reset unless it carries one itself.
 */
int32_t input(packet* pkt);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_TCP_H
