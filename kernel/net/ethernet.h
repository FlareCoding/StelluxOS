#ifndef STELLUX_NET_ETHERNET_H
#define STELLUX_NET_ETHERNET_H

#include "common/types.h"
#include "net/net.h"

namespace net {

constexpr uint16_t ETH_TYPE_ARP  = 0x0806;
constexpr uint16_t ETH_TYPE_IPV4 = 0x0800;

struct eth_header {
    uint8_t  dst[MAC_ADDR_LEN];
    uint8_t  src[MAC_ADDR_LEN];
    uint16_t ethertype; // network byte order
} __attribute__((packed));

static_assert(sizeof(eth_header) == 14, "eth_header must be 14 bytes");

constexpr uint8_t ETH_BROADCAST[MAC_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/**
 * Process a received Ethernet frame. Dispatches to ARP or IPv4 based on ethertype.
 */
void eth_recv(netif* iface, const uint8_t* data, size_t len);

/**
 * Send an Ethernet frame. Prepends the Ethernet header and calls iface->transmit().
 * @param iface     Interface to send on.
 * @param dst_mac   Destination MAC address (6 bytes).
 * @param ethertype Ethertype in HOST byte order.
 * @param payload   Payload data (after Ethernet header).
 * @param payload_len Length of payload.
 * @return 0 on success, negative error code on failure.
 */
int32_t eth_send(netif* iface, const uint8_t* dst_mac,
                 uint16_t ethertype, const uint8_t* payload, size_t payload_len);

} // namespace net

#endif // STELLUX_NET_ETHERNET_H
