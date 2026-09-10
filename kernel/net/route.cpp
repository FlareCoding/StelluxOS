#include "net/route.h"
#include "net/interface.h"

namespace net {
namespace route {

static bool is_local_route(const ipv4::ipv4_addr& dest, route_result* out) {
    interface* lo = find_loopback_interface();
    if (!lo) {
        return false;
    }

    *out = { lo, dest, dest, route_type::local };
    return true;
}

// Matches `dest` against what `iface` reaches directly, ignoring its gateway
static bool match_link(interface* iface, const ipv4::ipv4_addr& dest, route_result* out) {
    ipv4::ipv4_config conf = iface->ipv4_conf();
    if (!conf.configured()) {
        return false;
    }

    if (dest == conf.address) {
        return is_local_route(dest, out);
    }

    // The limited broadcast needs a link to carry it, and loopback has none
    if ((dest.is_broadcast() && !iface->is_loopback()) || conf.is_subnet_broadcast(dest)) {
        *out = { iface, dest, conf.address, route_type::broadcast };
        return true;
    }

    if (!dest.in_same_subnet(conf.address, conf.netmask)) {
        return false;
    }

    // Every address on the loopback network is this host
    if (iface->is_loopback()) {
        return is_local_route(dest, out);
    }

    *out = { iface, dest, conf.address, route_type::unicast };
    return true;
}

static bool has_gateway(interface* iface) {
    ipv4::ipv4_config conf = iface->ipv4_conf();
    return conf.configured() && !conf.gateway.is_unspecified();
}

static void gateway_route(interface* iface, route_result* out) {
    ipv4::ipv4_config conf = iface->ipv4_conf();
    *out = { iface, conf.gateway, conf.address, route_type::unicast };
}

interface* default_interface() {
    size_t count = interface_count();
    for (size_t i = 0; i < count; i++) {
        if (has_gateway(interface_at(i))) {
            return interface_at(i);
        }
    }

    return nullptr;
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

    interface* via = default_interface();
    if (!via) {
        return ERR_NO_ROUTE;
    }

    gateway_route(via, out);
    return OK;
}

int32_t lookup_on(interface* iface, const ipv4::ipv4_addr& dest, route_result* out) {
    if (!iface || !out) {
        return ERR_INVALID;
    }

    if (dest.is_loopback() || find_interface_by_address(dest)) {
        return is_local_route(dest, out) ? OK : ERR_NO_ROUTE;
    }

    // An unconfigured interface can still broadcast, which is how it asks for an address
    if (!iface->ipv4_conf().configured()) {
        if (!dest.is_broadcast()) {
            return ERR_NO_ROUTE;
        }

        *out = { iface, dest, ipv4::UNSPECIFIED_ADDR, route_type::broadcast };
        return OK;
    }

    if (match_link(iface, dest, out)) {
        return OK;
    }

    if (!has_gateway(iface)) {
        return ERR_NO_ROUTE;
    }

    gateway_route(iface, out);
    return OK;
}

} // namespace route
} // namespace net
