#ifndef STELLUX_NET_ROUTE_H
#define STELLUX_NET_ROUTE_H

#include "common/types.h"
#include "net/ipv4.h"

namespace net {

class interface;

namespace route {

enum class route_type : uint8_t {
    local     = 0, // this host itself, delivered through the loopback interface
    unicast   = 1, // a single host, reached through `next_hop`
    broadcast = 2, // every host on the interface's link
};

struct route_result {
    interface*      iface;    // Interface the packet leaves through
    ipv4::ipv4_addr next_hop; // The destination when on-link, otherwise the gateway
    ipv4::ipv4_addr source;   // Address the packet is sent from, unspecified while acquiring one
    route_type      type;
};

/*
 * Chooses how to reach `dest` from any interface.
 * Returns ERR_NO_ROUTE when none can.
 */
int32_t lookup(const ipv4::ipv4_addr& dest, route_result* out);

/*
 * Same purpose as `lookup`, but restricted to `iface` for callers that must send
 * from a specific interface, such as one requesting an address before it has one.
 */
int32_t lookup_on(interface* iface, const ipv4::ipv4_addr& dest, route_result* out);

/*
 * The interface whose gateway carries traffic no link reaches directly, or nullptr.
 */
interface* default_interface();

} // namespace route
} // namespace net

#endif // STELLUX_NET_ROUTE_H
