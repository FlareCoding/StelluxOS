#ifndef DHCPC_LEASE_HPP
#define DHCPC_LEASE_HPP

#include "message.hpp"

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <optional>

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

    /* Shortens the lease, with the renewal and rebinding times following it */
    void cap_to(uint32_t seconds);

    bool same_interface_config(const dhcp_lease& other) const;
    bool same_dns(const dhcp_lease& other) const;

    int apply(const char* iface) const;
    int publish_dns() const;
    unsigned prefix_length() const;

    static int clear_interface(const char* iface);

private:
    void derive_timers(std::optional<uint32_t> renewal, std::optional<uint32_t> rebinding);
};

#endif // DHCPC_LEASE_HPP
