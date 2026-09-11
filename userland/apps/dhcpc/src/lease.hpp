#ifndef DHCPC_LEASE_HPP
#define DHCPC_LEASE_HPP

#include "message.hpp"

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>

constexpr size_t DHCP_MAX_DNS = 3;

/* What a server proposes for one interface, as an OFFER or ACK carries it */
struct dhcp_lease {
    in_addr  address{};
    in_addr  netmask{};
    in_addr  router{};
    in_addr  server{};
    in_addr  dns[DHCP_MAX_DNS]{};
    size_t   dns_count = 0;
    uint32_t lease_seconds = 0;
    uint32_t renewal_seconds = 0;
    uint32_t rebinding_seconds = 0;

    /* False when the proposal could not be used on this host */
    bool from_message(const dhcp_message& msg);

    int apply(const char* iface) const;
    int publish_dns() const;
    unsigned prefix_length() const;
};

#endif // DHCPC_LEASE_HPP
