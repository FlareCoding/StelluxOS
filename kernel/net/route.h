#ifndef STELLUX_NET_ROUTE_H
#define STELLUX_NET_ROUTE_H

#include "common/types.h"
#include "net/net.h"

namespace net {

/**
 * Route type — determines how a packet is delivered.
 */
enum class route_type : uint8_t {
    LOCAL,       // Destination is a local address → deliver via loopback
    CONNECTED,   // Destination is directly reachable on this interface
    GATEWAY,     // Destination is reachable via a next-hop gateway
};

/**
 * A single entry in the routing table.
 */
struct route_entry {
    uint32_t    dest;       // destination network, host byte order
    uint32_t    netmask;    // network mask, host byte order
    uint32_t    gateway;    // next hop IP (0 for connected/local), host byte order
    netif*      iface;      // outgoing interface
    route_type  type;       // LOCAL, CONNECTED, GATEWAY
    uint8_t     _pad[1];
    uint16_t    metric;     // lower = preferred
    bool        valid;      // slot in use
    uint8_t     _pad2[3];
};

/**
 * Result of a route lookup.
 */
struct route_result {
    netif*      iface;      // outgoing interface
    uint32_t    next_hop;   // IP to ARP resolve (gateway or dst)
    route_type  type;       // LOCAL, CONNECTED, GATEWAY
};

constexpr uint32_t ROUTE_TABLE_SIZE = 32;

// Metric constants — lower = higher priority
constexpr uint16_t METRIC_LOCAL     = 0;      // own IPs → loopback
constexpr uint16_t METRIC_CONNECTED = 100;    // directly connected subnets
constexpr uint16_t METRIC_STATIC    = 200;    // manually added routes
constexpr uint16_t METRIC_DEFAULT   = 1024;   // default gateway

/**
 * Initialize the routing table. Called from net::init().
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void route_init();

/**
 * Add a route to the routing table.
 * @return net::OK on success, ERR_NOMEM if table is full, ERR_INVAL on bad args.
 */
int32_t route_add(uint32_t dest, uint32_t netmask, uint32_t gateway,
                  netif* iface, route_type type, uint16_t metric);

/**
 * Remove all routes associated with a given interface.
 * Called when an interface is unregistered or reconfigured.
 */
void route_del_iface(netif* iface);

/**
 * Remove a LOCAL host route (/32) for a specific IP address.
 * Used during interface reconfiguration to clean up the old LOCAL route
 * which points to loopback (not the reconfigured interface itself).
 * @param ip Host IP address in host byte order.
 */
void route_del_host(uint32_t ip);

/**
 * Lookup a route for a destination IP (host byte order).
 * Uses longest prefix match, then lowest metric for ties.
 * @param dst_ip Destination IP in host byte order.
 * @param result Output: interface, next-hop, route type.
 * @return net::OK if a route was found, ERR_NOIF if no route matches.
 */
int32_t route_lookup(uint32_t dst_ip, route_result* result);

/**
 * Auto-populate routes for a newly configured interface.
 * Adds:
 *   - LOCAL host route for the interface's own IP (→ loopback)
 *   - CONNECTED subnet route for the interface's subnet
 *   - GATEWAY default route if gateway != 0
 *
 * Called from net::configure() after setting interface IP config.
 */
void route_add_interface_routes(netif* iface);

/**
 * Count the number of valid routes in the table. For testing/debugging.
 */
uint32_t route_count();

} // namespace net

#endif // STELLUX_NET_ROUTE_H
