#ifndef STELLUX_NET_IPV4_H
#define STELLUX_NET_IPV4_H

#include "common/types.h"
#include "common/string.h"

namespace net {
namespace ipv4 {

constexpr size_t ADDR_LEN = 4;

/**
 * A 32-bit IPv4 address held as its four wire bytes, most significant first,
 * so it copies straight into and out of headers and there is never a byte
 * order to remember. `bytes[0]` is the first octet of the dotted notation.
 */
struct ipv4_addr {
    uint8_t bytes[ADDR_LEN];

    bool operator==(const ipv4_addr& other) const {
        return string::memcmp(bytes, other.bytes, ADDR_LEN) == 0;
    }

    bool operator!=(const ipv4_addr& other) const { return !(*this == other); }

    // 0.0.0.0, no address assigned
    bool is_unspecified() const;

    // 255.255.255.255, delivered to every host on the link
    bool is_broadcast() const;

    // True when this address and `other` share the network that `mask` describes
    bool in_same_subnet(const ipv4_addr& other, const ipv4_addr& mask) const;
} __attribute__((packed));
static_assert(sizeof(ipv4_addr) == ADDR_LEN);

constexpr ipv4_addr UNSPECIFIED_ADDR = {{0, 0, 0, 0}};
constexpr ipv4_addr BROADCAST_ADDR   = {{255, 255, 255, 255}};

inline bool ipv4_addr::is_unspecified() const { return *this == UNSPECIFIED_ADDR; }
inline bool ipv4_addr::is_broadcast() const { return *this == BROADCAST_ADDR; }

inline bool ipv4_addr::in_same_subnet(const ipv4_addr& other, const ipv4_addr& mask) const {
    for (size_t i = 0; i < ADDR_LEN; i++) {
        if ((bytes[i] & mask.bytes[i]) != (other.bytes[i] & mask.bytes[i])) {
            return false;
        }
    }

    return true;
}

/**
 * Network layer identity of one interface. `address` stays unspecified until
 * configuration assigns one, by hand or through DHCP, and an interface without
 * an address handles no network layer traffic, ARP included.
 */
struct ipv4_config {
    ipv4_addr address;
    ipv4_addr netmask;
    ipv4_addr gateway; // Router for destinations outside the subnet, unspecified if none

    bool configured() const { return !address.is_unspecified(); }
};

} // namespace ipv4
} // namespace net

#endif // STELLUX_NET_IPV4_H
