#ifndef STELLUX_NET_NETINFO_H
#define STELLUX_NET_NETINFO_H

#include "common/types.h"

namespace net {

constexpr uint32_t STLX_SIOCGNETSTATUS = 0x4E01;

constexpr uint32_t IFF_UP        = (1u << 0);
constexpr uint32_t IFF_CONFIGURED = (1u << 1);
constexpr uint32_t IFF_DEFAULT   = (1u << 2);

struct net_status_entry {
    char     name[16];
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ipv4_addr;    // host byte order
    uint32_t ipv4_netmask; // host byte order
    uint32_t ipv4_gateway; // host byte order
    uint32_t ipv4_dns;     // host byte order
    uint32_t flags;        // IFF_* bitmask
};

static_assert(sizeof(net_status_entry) == 44, "net_status_entry ABI size mismatch");

struct net_status {
    uint32_t         if_count;
    uint32_t         _reserved;
    net_status_entry interfaces[8];
};

static_assert(sizeof(net_status) == 360, "net_status ABI size mismatch");

/**
 * Query the status of all registered network interfaces.
 * Fills out->interfaces[] with identity, IPv4 config, and live link
 * status for each registered interface. Sets IFF_DEFAULT on the
 * default outbound interface.
 * @note Privilege: **required** (calls driver link_up callbacks)
 */
__PRIVILEGED_CODE int32_t query_status(net_status* out);

} // namespace net

#endif // STELLUX_NET_NETINFO_H
