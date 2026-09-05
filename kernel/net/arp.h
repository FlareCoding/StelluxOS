#ifndef STELLUX_NET_ARP_H
#define STELLUX_NET_ARP_H

#include "net/eth.h"
#include "net/ipv4.h"

namespace net {
namespace arp {

constexpr size_t HEADER_LEN = 28;

constexpr uint16_t HW_TYPE_ETHERNET = 1;
constexpr uint16_t PROTO_TYPE_IPV4  = eth::TYPE_IPV4;
constexpr uint16_t OP_REQUEST       = 1;
constexpr uint16_t OP_REPLY         = 2;

// ARP table configuration
constexpr uint64_t NS_PER_SEC          = 1000000000ULL;
constexpr size_t   TABLE_SIZE          = 16;
constexpr uint64_t ENTRY_LIFETIME_NS   = 300 * NS_PER_SEC; // resolved entries expire after this
constexpr uint64_t PENDING_TIMEOUT_NS  = 5 * NS_PER_SEC;   // unanswered requests fail after this
constexpr size_t   PENDING_QUEUE_DEPTH = 3;                // packets held per unresolved entry

/**
 * ARP packet for Ethernet over IPv4 (RFC 826). The standard header is generic,
 * which is what `hw_len` and `proto_len` describe, but only this one shape is
 * supported by Stellux, so the addresses are typed and the lengths are checked on
 * input rather than interpreted. Multibyte fields are in network byte order.
 * https://www.rfc-editor.org/info/rfc826/
 */
struct arp_header {
    uint16_t        hw_type;           // HW_TYPE_ETHERNET
    uint16_t        proto_type;        // PROTO_TYPE_IPV4
    uint8_t         hw_len;            // eth::MAC_ADDR_LEN
    uint8_t         proto_len;         // ipv4::ADDR_LEN
    uint16_t        opcode;            // OP_REQUEST or OP_REPLY
    eth::mac_addr   sender_hw_addr;    // Sender MAC address
    ipv4::ipv4_addr sender_proto_addr; // Sender IPv4 address
    eth::mac_addr   target_hw_addr;    // Destination MAC, zero in a request
    ipv4::ipv4_addr target_proto_addr; // Destination address being resolved
} __attribute__((packed));
static_assert(sizeof(arp_header) == HEADER_LEN);

/**
 * Lifecycle of a table entry. An entry is `pending` from the moment a request goes
 * out until the reply arrives, and packets for that address wait on it in the
 * meantime. It is `resolved` while the hardware address is known and not expired.
 * `empty` is zero so that zeroed memory is a free slot in the table.
 */
enum class arp_entry_state : uint8_t {
    empty    = 0,
    pending  = 1,
    resolved = 2,
};

/*
 * Consumes an ARP packet. A request for this host's
 * address is answered in place, everything else is freed.
 */
int32_t input(packet* pkt);

/*
 * Consumes a finished ARP packet and targets it for `dest`,
 * the requester's address for a reply or broadcast for a request.
 */
int32_t output(packet* pkt, const eth::mac_addr& dest);

/*
 * Ages the table on every daemon pass. `ts` is the current monotonic time.
 */
void sweep(uint64_t ts);

} // namespace arp
} // namespace net

#endif // STELLUX_NET_ARP_H
