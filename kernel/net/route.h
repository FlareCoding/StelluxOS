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
    netif*      owner;      // interface whose configure() created this route
                            // (may differ from iface for LOCAL routes that
                            // point to loopback but belong to another iface)
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
 * @param owner The interface whose configure() call created this route.
 *              For LOCAL routes, owner is the configured interface while
 *              iface is loopback. For other routes, owner == iface.
 *              If nullptr, defaults to iface.
 * @return net::OK on success, ERR_NOMEM if table is full, ERR_INVAL on bad args.
 */
int32_t route_add(uint32_t dest, uint32_t netmask, uint32_t gateway,
                  netif* iface, route_type type, uint16_t metric,
                  netif* owner = nullptr);

/**
 * Remove all routes owned by the given interface.
 * Matches on the owner field, not the outgoing interface. This ensures
 * that LOCAL routes (which point to loopback) are correctly removed
 * when their owning interface is unregistered or reconfigured, without
 * accidentally removing LOCAL routes belonging to other interfaces.
 */
void route_del_iface(netif* iface);

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
