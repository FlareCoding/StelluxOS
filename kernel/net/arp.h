#ifndef STELLUX_NET_ARP_H
#define STELLUX_NET_ARP_H

#include "common/list.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/packet.h"
#include "sync/spinlock.h"

namespace net {

class interface;

namespace arp {

constexpr size_t HEADER_LEN = 28;

constexpr uint16_t HW_TYPE_ETHERNET = 1;
constexpr uint16_t PROTO_TYPE_IPV4  = eth::TYPE_IPV4;
constexpr uint16_t OP_REQUEST       = 1;
constexpr uint16_t OP_REPLY         = 2;

// ARP table configuration
constexpr uint64_t NS_PER_SEC           = 1000000000ULL;
constexpr size_t   TABLE_SIZE           = 16;
constexpr uint64_t ENTRY_LIFETIME_NS    = 300 * NS_PER_SEC; // resolved entries expire after this
constexpr uint64_t REQUEST_RETRY_NS     = 1 * NS_PER_SEC;   // pending entries resend their request at this interval
constexpr uint8_t  MAX_REQUEST_ATTEMPTS = 3;                // pending entries fail after this many requests
constexpr size_t   PENDING_QUEUE_DEPTH  = 3;                // packets held per unresolved entry

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

struct arp_entry {
    interface*      iface;
    arp_entry_state state;
    uint8_t         attempts;  // Requests sent while pending
    ipv4::ipv4_addr ip;
    eth::mac_addr   mac;
    uint64_t        timestamp; // Last request sent while pending, last confirmed while resolved
    packet_list     queue;
};

// Answer from the table when asked how to send to an address: the MAC is known
// and the caller transmits now, or the table has stored the packet in a pending entry.
enum class arp_send_action : uint8_t {
    transmit = 0,
    queued   = 1,
};

struct arp_retry {
    interface*      iface;
    ipv4::ipv4_addr ip;
};

class arp_table {
public:
    arp_table() = default;
    ~arp_table() = default;

    void init();

    /*
     * Fills `out_mac` when `ip` is known, otherwise holds `pkt` on a pending entry and
     * sets `send_request` when one must go out. Packets evicted for room return in `dropped`.
     */
    arp_send_action resolve(
        interface* iface,
        const ipv4::ipv4_addr& ip,
        packet* pkt,
        uint64_t timestamp,
        eth::mac_addr* out_mac,
        bool* send_request,
        packet_list& dropped
    );

    /*
     * Refreshes the entry for `ip`, creating it only when `create` is set. Packets that
     * waited on it return in `flushed` for sending. True when a pending entry resolved.
     */
    bool update_entry(
        interface* iface,
        const ipv4::ipv4_addr& ip,
        const eth::mac_addr& mac,
        uint64_t timestamp,
        bool create,
        packet_list& flushed
    );

    /*
     * Expires resolved entries past their lifetime and retries or fails pending ones.
     * Failed entries return their packets in `dropped`, ones to retry are listed in `retries`.
     */
    void sweep(
        uint64_t timestamp,
        packet_list& dropped,
        arp_retry* retries,
        size_t* retry_count
    );

private:
    arp_entry* find_entry(interface* iface, const ipv4::ipv4_addr& ip);
    arp_entry* take_free_entry();
    arp_entry* allocate_entry(packet_list& dropped);
    void clear_entry(arp_entry& entry);

    sync::spinlock m_lock;
    arp_entry      m_entries[TABLE_SIZE];
};

/*
 * Sets up the ARP table
 */
int32_t init();

/*
 * Consumes an ARP packet.
 */
int32_t input(packet* pkt);

/*
 * Consumes a finished ARP packet and targets it for `dest`,
 * the requester's address for a reply or broadcast for a request.
 */
int32_t output(packet* pkt, const eth::mac_addr& dest);

/*
 * Fills `out` mac address and returns OK when `ip` is resolved,
 * otherwise requests it and returns ERR_PENDING.
 */
int32_t resolve(interface* iface, const ipv4::ipv4_addr& ip, eth::mac_addr* out);

/*
 * Consumes an IPv4 packet and sends it to the unicast `next_hop` on the
 * packet's interface, holding it in the table until the address resolves.
 */
int32_t resolve_and_send(packet* pkt, const ipv4::ipv4_addr& next_hop);

/*
 * Ages the table on every netstkd daemon pass. `ts` is the current monotonic time.
 */
void sweep(uint64_t ts);

} // namespace arp
} // namespace net

#endif // STELLUX_NET_ARP_H
