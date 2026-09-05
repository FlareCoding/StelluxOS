#ifndef STELLUX_NET_ETH_H
#define STELLUX_NET_ETH_H

#include "common/types.h"
#include "common/string.h"
#include "net/packet.h"
#include "net/byteorder.h"

namespace net {
namespace eth {

constexpr size_t MAC_ADDR_LEN    = 6;
constexpr size_t HEADER_LEN      = 14;
constexpr size_t MTU             = 1500;                        // largest payload in one frame
constexpr size_t MIN_FRAME_LEN   = 60;                          // shorter frames arrive padded
constexpr size_t MAX_FRAME_LEN   = HEADER_LEN + MTU;            // 1514
constexpr size_t MIN_PAYLOAD_LEN = MIN_FRAME_LEN - HEADER_LEN;  // 46

// Headroom a driver reserves before copying a received frame in. A 14-byte
// header leaves the network header that follows it misaligned, two bytes of
// padding put it back on a 4-byte boundary.
constexpr size_t RX_ALIGN_PAD = 2;

// Values of the type field, in host byte order. Frames with a value below
// TYPE_MIN carry an IEEE 802.3 payload length there instead of a type.
constexpr uint16_t TYPE_MIN  = 0x0600;
constexpr uint16_t TYPE_IPV4 = 0x0800;
constexpr uint16_t TYPE_ARP  = 0x0806;
constexpr uint16_t TYPE_IPV6 = 0x86DD;

struct mac_addr {
    uint8_t bytes[MAC_ADDR_LEN];

    bool operator==(const mac_addr& other) const {
        return string::memcmp(bytes, other.bytes, MAC_ADDR_LEN) == 0;
    }

    bool operator!=(const mac_addr& other) const { return !(*this == other); }

    bool is_multicast() const { return (bytes[0] & 0x01) != 0; }
    bool is_broadcast() const;
} __attribute__((packed));

static_assert(sizeof(mac_addr) == MAC_ADDR_LEN);

constexpr mac_addr BROADCAST_ADDR = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

inline bool mac_addr::is_broadcast() const { return *this == BROADCAST_ADDR; }

/**
 * Ethernet II frame header (RFC 894, IEEE 802.3 Clause 3.1.1).
 * `type` is stored in network byte order, so it must be read through `ntohs`
 * and written through `htons`.
 * https://www.rfc-editor.org/info/rfc894/
 */
struct eth_header {
    mac_addr dest;  // Station the frame is for, or the broadcast address
    mac_addr src;   // Station that sent the frame
    uint16_t type;  // Type of the payload
} __attribute__((packed));
static_assert(sizeof(eth_header) == HEADER_LEN);

/*
 * Entry point for the ethernet layer of the network stack to consume
 * the packet. Refuses frames shorter than the header, marks the link
 * header, pulls it, and dispatches on the type. Anything not handled is
 * counted and freed here.
 */
int32_t input(packet* pkt);

/*
 * Exit point for the ethernet layer of the network stack to consume
 * the packet. Refuses packets without headroom for the header or without
 * an interface to leave through, pushes the header with the source taken
 * from that interface, and hands the frame to it for transmission. The
 * packet is freed here whether or not the interface accepted it.
 */
int32_t output(packet* pkt, const mac_addr& dest, uint16_t type);

} // namespace eth
} // namespace net

#endif // STELLUX_NET_ETH_H
