#ifndef STELLUX_NET_ARP_H
#define STELLUX_NET_ARP_H

#include "common/types.h"
#include "net/net.h"

namespace net {

constexpr uint16_t ARP_HW_ETHERNET = 1;
constexpr uint16_t ARP_OP_REQUEST  = 1;
constexpr uint16_t ARP_OP_REPLY    = 2;

constexpr uint32_t ARP_TABLE_SIZE  = 32;
constexpr uint32_t ARP_RETRY_COUNT = 3;
constexpr uint32_t ARP_RETRY_DELAY_MS = 100;

struct arp_header {
    uint16_t hw_type;      // network byte order
    uint16_t proto_type;   // network byte order
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;       // network byte order
    uint8_t  sender_mac[MAC_ADDR_LEN];
    uint32_t sender_ip;    // network byte order
    uint8_t  target_mac[MAC_ADDR_LEN];
    uint32_t target_ip;    // network byte order
} __attribute__((packed));

static_assert(sizeof(arp_header) == 28, "arp_header must be 28 bytes");

/**
 * Initialize the ARP table.
 */
void arp_init();

/**
 * Process a received ARP packet (after Ethernet header is stripped).
 */
void arp_recv(netif* iface, const uint8_t* data, size_t len);

/**
 * Resolve an IPv4 address (host byte order) to a MAC address.
 * May block briefly if an ARP request needs to be sent.
 * @return 0 on success (out_mac filled), negative on failure.
 */
int32_t arp_resolve(netif* iface, uint32_t target_ip, uint8_t* out_mac);

/**
 * Send an ARP request for the given IP (host byte order).
 */
void arp_send_request(netif* iface, uint32_t target_ip);

} // namespace net

#endif // STELLUX_NET_ARP_H
