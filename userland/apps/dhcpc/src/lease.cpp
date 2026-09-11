#include "lease.hpp"

#include <stlx/net.h>

#include <arpa/inet.h>

#include <cstdio>
#include <cstring>

constexpr const char* RESOLV_CONF_PATH = "/etc/resolv.conf";
constexpr uint32_t    MAX_LEASE_SECONDS = 7 * 24 * 3600;

static in_addr to_address(std::span<const uint8_t> bytes) {
    in_addr address;
    memcpy(&address.s_addr, bytes.data(), sizeof(address.s_addr));
    return address;
}

static std::optional<in_addr> find_address(const dhcp_message& msg, uint8_t code) {
    auto value = msg.find(code);
    if (!value || value->size() < sizeof(in_addr::s_addr) || value->size() % sizeof(in_addr::s_addr) != 0) {
        return std::nullopt;
    }

    return to_address(*value);
}

static std::optional<uint32_t> find_u32(const dhcp_message& msg, uint8_t code) {
    auto value = msg.find(code);
    if (!value || value->size() != sizeof(uint32_t)) {
        return std::nullopt;
    }

    return ntohl(to_address(*value).s_addr);
}

/* A host this client could talk to: not unspecified, loopback, multicast or the limited broadcast */
static bool is_usable_host(uint32_t host_order) {
    return host_order != 0 && (host_order >> 24) != 127 && (host_order >> 28) != 0xE && host_order != 0xFFFFFFFF;
}

static bool is_contiguous_mask(uint32_t host_order) {
    uint32_t inverted = ~host_order;
    return host_order != 0 && (inverted & (inverted + 1)) == 0;
}

bool dhcp_lease::from_message(const dhcp_message& msg) {
    address = msg.yiaddr();
    uint32_t host = ntohl(address.s_addr);
    if (!is_usable_host(host)) {
        return false;
    }

    auto server_id = find_address(msg, OPT_SERVER_ID);
    if (!server_id || !is_usable_host(ntohl(server_id->s_addr))) {
        return false;
    }

    auto mask = find_address(msg, OPT_SUBNET_MASK);
    if (!mask || !is_contiguous_mask(ntohl(mask->s_addr))) {
        return false;
    }

    /* The network and broadcast addresses of the offered subnet are not hosts */
    uint32_t host_bits = ~ntohl(mask->s_addr);
    if (host_bits > 1 && ((host & host_bits) == 0 || (host & host_bits) == host_bits)) {
        return false;
    }

    auto lease = find_u32(msg, OPT_LEASE_TIME);
    if (!lease || *lease == 0) {
        return false;
    }

    server = *server_id;
    netmask = *mask;
    router = find_address(msg, OPT_ROUTER).value_or(in_addr{});

    dns_count = 0;
    if (auto servers = msg.find(OPT_DNS)) {
        for (size_t at = 0; at + sizeof(in_addr::s_addr) <= servers->size() && dns_count < DHCP_MAX_DNS; at += sizeof(in_addr::s_addr)) {
            in_addr candidate = to_address(servers->subspan(at, sizeof(in_addr::s_addr)));
            if (is_usable_host(ntohl(candidate.s_addr))) {
                dns[dns_count++] = candidate;
            }
        }
    }

    lease_seconds = *lease < MAX_LEASE_SECONDS ? *lease : MAX_LEASE_SECONDS;
    derive_timers(find_u32(msg, OPT_RENEWAL_TIME), find_u32(msg, OPT_REBINDING_TIME));
    return true;
}

void dhcp_lease::cap_to(uint32_t seconds) {
    if (seconds == 0 || lease_seconds <= seconds) {
        return;
    }

    lease_seconds = seconds;
    derive_timers(std::nullopt, std::nullopt);
}

/* The server's times are kept only when they divide the lease the way RFC 2131 orders them */
void dhcp_lease::derive_timers(std::optional<uint32_t> renewal, std::optional<uint32_t> rebinding) {
    uint32_t default_rebinding = static_cast<uint32_t>(static_cast<uint64_t>(lease_seconds) * 7 / 8);
    renewal_seconds = renewal.value_or(lease_seconds / 2);
    rebinding_seconds = rebinding.value_or(default_rebinding);
    if (renewal_seconds >= lease_seconds || rebinding_seconds >= lease_seconds || rebinding_seconds <= renewal_seconds) {
        renewal_seconds = lease_seconds / 2;
        rebinding_seconds = default_rebinding;
    }
}

bool dhcp_lease::same_interface_config(const dhcp_lease& other) const {
    return address.s_addr == other.address.s_addr && netmask.s_addr == other.netmask.s_addr &&
           router.s_addr == other.router.s_addr;
}

bool dhcp_lease::same_dns(const dhcp_lease& other) const {
    if (dns_count != other.dns_count) {
        return false;
    }

    for (size_t i = 0; i < dns_count; i++) {
        if (dns[i].s_addr != other.dns[i].s_addr) {
            return false;
        }
    }

    return true;
}

int dhcp_lease::apply(const char* iface) const {
    stlx_ifconf conf = {};
    snprintf(conf.name, sizeof(conf.name), "%s", iface);
    conf.ipv4_addr = ntohl(address.s_addr);
    conf.ipv4_netmask = ntohl(netmask.s_addr);
    conf.ipv4_gateway = ntohl(router.s_addr);
    return stlx_net_set_config(&conf);
}

int dhcp_lease::publish_dns() const {
    if (dns_count == 0) {
        return 0;
    }

    FILE* f = fopen(RESOLV_CONF_PATH, "w");
    if (!f) {
        return -1;
    }

    for (size_t i = 0; i < dns_count; i++) {
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &dns[i], text, sizeof(text));
        fprintf(f, "nameserver %s\n", text);
    }

    fclose(f);
    return 0;
}

unsigned dhcp_lease::prefix_length() const {
    return static_cast<unsigned>(__builtin_popcount(ntohl(netmask.s_addr)));
}

int dhcp_lease::clear_interface(const char* iface) {
    stlx_ifconf conf = {};
    snprintf(conf.name, sizeof(conf.name), "%s", iface);
    return stlx_net_set_config(&conf);
}
