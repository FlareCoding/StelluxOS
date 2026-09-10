#ifndef STELLUX_NET_IPV4_H
#define STELLUX_NET_IPV4_H

#include "net/packet.h"
#include "net/byteorder.h"
#include "common/string.h"

namespace net {
namespace ipv4 {

constexpr size_t ADDR_LEN = 4;

constexpr size_t   HEADER_LEN           = 20;
constexpr size_t   MAX_HEADER_LEN       = 60;
constexpr size_t   PSEUDO_HEADER_LEN    = 12;

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

    // 224.0.0.0/4, delivered to a group of hosts
    bool is_multicast() const;

    // 240.0.0.0/4, reserved (RFC 1112), names no host except as the broadcast above
    bool is_reserved() const;

    // 127.0.0.0/8, never leaves the host
    bool is_loopback() const;

    // 0.0.0.0/8, a host that does not have its address yet
    bool in_zero_network() const;

    // True when this address and `other` share the network that `mask` describes
    bool in_same_subnet(const ipv4_addr& other, const ipv4_addr& mask) const;

    // True for a netmask of one or more leading ones followed by zeros
    bool is_contiguous_mask() const;
} __attribute__((packed));
static_assert(sizeof(ipv4_addr) == ADDR_LEN);

constexpr ipv4_addr UNSPECIFIED_ADDR = {{0, 0, 0, 0}};
constexpr ipv4_addr BROADCAST_ADDR   = {{255, 255, 255, 255}};

inline bool ipv4_addr::is_unspecified() const { return *this == UNSPECIFIED_ADDR; }
inline bool ipv4_addr::is_broadcast() const { return *this == BROADCAST_ADDR; }
inline bool ipv4_addr::is_multicast() const { return (bytes[0] & 0xF0) == 0xE0; }
inline bool ipv4_addr::is_reserved() const { return (bytes[0] & 0xF0) == 0xF0; }
inline bool ipv4_addr::is_loopback() const { return bytes[0] == 127; }
inline bool ipv4_addr::in_zero_network() const { return bytes[0] == 0; }

inline bool ipv4_addr::is_contiguous_mask() const {
    uint32_t mask = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
                    (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
    return mask != 0 && (mask | (mask - 1)) == 0xFFFFFFFFu;
}

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

    // True for the current subnet's broadcast in either form, all host bits set
    // or the obsolete all clear (RFC 1122 3.3.6). A /31 or /32 has no broadcast.
    bool is_subnet_broadcast(const ipv4_addr& addr) const;

    // True when `addr` names exactly one host as seen from this interface (RFC 1122
    // 3.2.1.3). Loopback addresses count only on the loopback interface itself.
    bool is_unicast(const ipv4_addr& addr) const;

    // True when the address is a host on the subnet the mask describes, and the
    // gateway is either unspecified or is a different host on the same subnet.
    bool is_well_formed() const;
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

/**
 * Twelve bytes UDP and TCP include in their checksum but never send (RFC 768,
 * RFC 793). Covering the addresses lets a receiver detect a segment or datagram
 * that was delivered to the wrong host. `length` is the transport header and payload.
 */
struct pseudo_header {
    ipv4_addr src;
    ipv4_addr dst;
    uint8_t   zero;
    uint8_t   proto;
    uint16_t  length;
} __attribute__((packed));
static_assert(sizeof(pseudo_header) == PSEUDO_HEADER_LEN);

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
