#ifndef STELLUX_NET_TCP_TCP_H
#define STELLUX_NET_TCP_TCP_H

#include "net/packet.h"
#include "net/ipv4.h"

namespace net {
namespace tcp {

/**
 * @brief Prepares the TCP layer
 */
int32_t init();

/**
 * @brief Consumes a segment whose window starts at the TCP header with the
 * interface set and the IPv4 header marked. A segment no connection claims is
 * answered with a reset unless it carries one itself.
 */
int32_t input(packet* pkt);

/**
 * @brief Takes an ICMP error about a segment this host sent: `inner` is the IP
 * header it carried, `segment` the start of the segment behind it. A handshake
 * fails with the error, a synchronized connection records it (RFC 1122 4.2.3.9).
 */
void icmp_error(uint8_t type, uint8_t code, const ipv4::ipv4_header* inner, const uint8_t* segment,
                size_t segment_len);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_TCP_H
