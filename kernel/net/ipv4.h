#ifndef STELLUX_NET_IPV4_H
#define STELLUX_NET_IPV4_H

#include "net/packet.h"
#include "net/byteorder.h"
#include "common/string.h"

namespace net {
namespace ipv4 {

constexpr size_t ADDR_LEN = 4;

constexpr size_t   HEADER_LEN       = 20;
constexpr size_t   MAX_HEADER_LEN   = 60;

constexpr uint8_t  VERSION          = 4;
constexpr uint8_t  MIN_IHL          = 5;
constexpr uint8_t  DEFAULT_TTL      = 64;

constexpr uint8_t  PROTO_ICMP       = 1;
constexpr uint8_t  PROTO_TCP        = 6;
constexpr uint8_t  PROTO_UDP        = 17;

constexpr uint16_t FLAG_DF          = 0x4000;
constexpr uint16_t FLAG_MF          = 0x2000;
constexpr uint16_t FRAG_OFFSET_MASK = 0x1FFF;

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

    // True for the all-host-bits address of the current subnet
    bool is_subnet_broadcast(const ipv4_addr& addr) const;
};

struct ipv4_header {
    uint8_t     version_ihl;
    uint8_t     tos;
    uint16_t    total_len;
    uint16_t    id;
    uint16_t    fl_frag_off;
    uint8_t     ttl;
    uint8_t     proto;
    uint16_t    checksum;
    ipv4_addr   src;
    ipv4_addr   dst;

    inline uint8_t version() const { return version_ihl >> 4; }
    inline uint8_t ihl() const { return version_ihl & 0x0F; }
    inline size_t header_len() const { return ihl() * sizeof(uint32_t); }

    void set_version_ihl(uint8_t version, uint8_t ihl) {
        version_ihl = static_cast<uint8_t>((version << 4) | (ihl & 0x0F));
    }

    inline uint16_t flags() const { return ntohs(fl_frag_off) & ~FRAG_OFFSET_MASK; }
    inline uint16_t frag_off() const { return ntohs(fl_frag_off) & FRAG_OFFSET_MASK; }

} __attribute__((packed));
static_assert(sizeof(ipv4_header) == HEADER_LEN);

/*
 * Entry point into the IP layer of the network stack
 */
int32_t input(packet* pkt);

/*
 * Transmits a constructed packet for a given `dest` ipv4 address
 */
int32_t output(packet* pkt, const ipv4_addr& dest, uint8_t protocol);


} // namespace ipv4
} // namespace net

#endif // STELLUX_NET_IPV4_H
