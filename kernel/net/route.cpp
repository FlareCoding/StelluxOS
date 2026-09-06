#include "net/route.h"
#include "net/interface.h"

namespace net {
namespace route {

// Matches `dest` against what `iface` reaches directly, ignoring its gateway
static bool match_link(interface* iface, const ipv4::ipv4_addr& dest, route_result* out) {
    const ipv4::ipv4_config& conf = iface->ipv4_conf();
    if (!conf.configured()) {
        return false;
    }

    if (dest == conf.address) {
        *out = { iface, dest, route_type::local };
        return true;
    }

    if (dest.is_broadcast() || conf.is_subnet_broadcast(dest)) {
        *out = { iface, dest, route_type::broadcast };
        return true;
    }

    if (dest.in_same_subnet(conf.address, conf.netmask)) {
        *out = { iface, dest, route_type::unicast };
        return true;
    }

    return false;
}

static bool match_gateway(interface* iface, route_result* out) {
    const ipv4::ipv4_config& conf = iface->ipv4_conf();
    if (!conf.configured() || conf.gateway.is_unspecified()) {
        return false;
    }

    *out = { iface, conf.gateway, route_type::unicast };
    return true;
}

int32_t lookup(const ipv4::ipv4_addr& dest, route_result* out) {
    if (!out) {
        return ERR_INVALID;
    }

    // Every link is tried before any gateway, so an on-link host is never
    // sent through a router
    size_t count = interface_count();
    for (size_t i = 0; i < count; i++) {
        if (match_link(interface_at(i), dest, out)) {
            return OK;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (match_gateway(interface_at(i), out)) {
            return OK;
        }
    }

    return ERR_NO_ROUTE;
}

int32_t lookup_on(interface* iface, const ipv4::ipv4_addr& dest, route_result* out) {
    if (!iface || !out) {
        return ERR_INVALID;
    }

    // An unconfigured interface can still broadcast, which is how it asks for an address
    if (!iface->ipv4_conf().configured()) {
        if (!dest.is_broadcast()) {
            return ERR_NO_ROUTE;
        }

        *out = { iface, dest, route_type::broadcast };
        return OK;
    }

    if (match_link(iface, dest, out) || match_gateway(iface, out)) {
        return OK;
    }

    return ERR_NO_ROUTE;
}

} // namespace route
} // namespace net
