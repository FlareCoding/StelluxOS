#include "net/route.h"
#include "net/loopback.h"
#include "common/logging.h"
#include "common/string.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

__PRIVILEGED_DATA static route_entry g_route_table[ROUTE_TABLE_SIZE] = {};
__PRIVILEGED_DATA static sync::spinlock g_route_lock = sync::SPINLOCK_INIT;

/**
 * Count the number of set bits in a 32-bit value.
 * Used for comparing prefix lengths (netmask bit count).
 */
static uint32_t popcount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    return (((v + (v >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

} // anonymous namespace

__PRIVILEGED_CODE void route_init() {
    for (uint32_t i = 0; i < ROUTE_TABLE_SIZE; i++) {
        g_route_table[i].valid = false;
    }
}

int32_t route_add(uint32_t dest, uint32_t netmask, uint32_t gateway,
                  netif* iface, route_type type, uint16_t metric,
                  netif* owner) {
    if (!iface) {
        return ERR_INVAL;
    }

    // Default owner to iface if not specified
    if (!owner) {
        owner = iface;
    }

    int32_t result = ERR_NOMEM;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_route_lock);

        for (uint32_t i = 0; i < ROUTE_TABLE_SIZE; i++) {
            if (!g_route_table[i].valid) {
                g_route_table[i].dest    = dest;
                g_route_table[i].netmask = netmask;
                g_route_table[i].gateway = gateway;
                g_route_table[i].iface   = iface;
                g_route_table[i].owner   = owner;
                g_route_table[i].type    = type;
                g_route_table[i].metric  = metric;
                g_route_table[i].valid   = true;
                result = OK;
                break;
            }
        }
    });

    return result;
}

void route_del_iface(netif* iface) {
    if (!iface) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_route_lock);

        // Match on the owner field so that LOCAL routes (which point to
        // loopback as their outgoing interface) are correctly associated
        // with the interface whose configure() created them.
        for (uint32_t i = 0; i < ROUTE_TABLE_SIZE; i++) {
            if (g_route_table[i].valid && g_route_table[i].owner == iface) {
                g_route_table[i].valid = false;
            }
        }
    });
}

void route_del_host(uint32_t ip) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_route_lock);

        for (uint32_t i = 0; i < ROUTE_TABLE_SIZE; i++) {
            if (g_route_table[i].valid &&
                g_route_table[i].type == route_type::LOCAL &&
                g_route_table[i].dest == ip &&
                g_route_table[i].netmask == 0xFFFFFFFF) {
                g_route_table[i].valid = false;
            }
        }
    });
}

int32_t route_lookup(uint32_t dst_ip, route_result* result) {
    if (!result) return ERR_INVAL;

    int32_t rc = ERR_NOIF;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_route_lock);

        bool found = false;
        uint32_t best_prefix_len = 0;
        uint16_t best_metric = 0xFFFF;
        uint32_t best_idx = 0;

        for (uint32_t i = 0; i < ROUTE_TABLE_SIZE; i++) {
            if (!g_route_table[i].valid) continue;

            // Check if destination matches this route's network
            if ((dst_ip & g_route_table[i].netmask) != g_route_table[i].dest) {
                continue;
            }

            uint32_t prefix_len = popcount32(g_route_table[i].netmask);

            // Prefer longest prefix match, then lowest metric
            if (!found ||
                prefix_len > best_prefix_len ||
                (prefix_len == best_prefix_len &&
                 g_route_table[i].metric < best_metric)) {
                best_prefix_len = prefix_len;
                best_metric = g_route_table[i].metric;
                best_idx = i;
                found = true;
            }
        }

        if (found) {
            const auto& best = g_route_table[best_idx];
            result->iface = best.iface;
            result->type  = best.type;

            switch (best.type) {
            case route_type::LOCAL:
                // Local delivery — next hop is the destination itself
                result->next_hop = dst_ip;
                break;
            case route_type::CONNECTED:
                // Directly reachable — next hop is the destination
                result->next_hop = dst_ip;
                break;
            case route_type::GATEWAY:
                // Via gateway — next hop is the gateway address
                result->next_hop = best.gateway;
                break;
            }

            rc = OK;
        }
    });

    return rc;
}

void route_add_interface_routes(netif* iface) {
    if (!iface || !iface->configured) return;

    netif* lo = get_loopback_netif();

    // Add a LOCAL host route for the interface's own IP (→ loopback).
    // This enables local delivery when sending to our own address.
    // The route's outgoing interface is loopback, but the owner is the
    // configured interface — so route_del_iface(iface) correctly cleans
    // it up without affecting other interfaces' LOCAL routes.
    // Skip this for the loopback interface itself — its CONNECTED route
    // (127.0.0.0/8) already covers local delivery.
    if (lo && iface != lo) {
        route_add(iface->ipv4_addr, 0xFFFFFFFF, 0,
                  lo, route_type::LOCAL, METRIC_LOCAL, iface);
    }

    // Add a CONNECTED subnet route for the interface's subnet.
    // This means: to reach any address in this subnet, send directly
    // on this interface (no gateway needed).
    uint32_t subnet = iface->ipv4_addr & iface->ipv4_netmask;
    route_add(subnet, iface->ipv4_netmask, 0,
              iface, route_type::CONNECTED, METRIC_CONNECTED);

    // If a gateway is configured, add a default route (0.0.0.0/0)
    // through the gateway via this interface.
    if (iface->ipv4_gateway != 0) {
        route_add(0, 0, iface->ipv4_gateway,
                  iface, route_type::GATEWAY, METRIC_DEFAULT);
    }
}

uint32_t route_count() {
    uint32_t count = 0;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_route_lock);
        for (uint32_t i = 0; i < ROUTE_TABLE_SIZE; i++) {
            if (g_route_table[i].valid) {
                count++;
            }
        }
    });

    return count;
}

} // namespace net
