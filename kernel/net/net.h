#ifndef STELLUX_NET_NET_H
#define STELLUX_NET_NET_H

#include "common/types.h"

namespace net {

constexpr int32_t OK        =  0;
constexpr int32_t ERR_INIT  = -1;
constexpr int32_t ERR_NOMEM = -2;
constexpr int32_t ERR_INVAL = -3;
constexpr int32_t ERR_NOIF  = -4;
constexpr int32_t ERR_TIMEOUT = -5;
constexpr int32_t ERR_NOARP   = -6;
constexpr int32_t ERR_NOREPLY = -7;

// Maximum Ethernet frame size (without FCS)
constexpr size_t ETH_FRAME_MAX = 1514;
constexpr size_t ETH_MTU       = 1500;
constexpr size_t MAC_ADDR_LEN  = 6;
constexpr size_t MAX_INTERFACES = 8;

// Interface flags
constexpr uint32_t NETIF_UP       = (1u << 0);  // administratively up
constexpr uint32_t NETIF_RUNNING  = (1u << 1);  // link is up (carrier detected)
constexpr uint32_t NETIF_LOOPBACK = (1u << 2);  // virtual loopback device

// Forward declaration
struct netif;

// Driver callback types
using netif_tx_fn   = int32_t (*)(netif* iface, const uint8_t* frame, size_t len);
using netif_link_fn = bool (*)(netif* iface);
using netif_poll_fn = void (*)(netif* iface);

/**
 * Network interface descriptor.
 * Drivers fill in identity + callbacks, then call register_netif().
 * The protocol stack manages everything else.
 */
struct netif {
    // --- Identity (set by driver before registration) ---
    char         name[16];
    uint8_t      mac[MAC_ADDR_LEN];

    // --- Driver callbacks (set by driver before registration) ---
    netif_tx_fn   transmit;    // send a raw Ethernet frame
    netif_link_fn link_up;     // query link status
    void*         driver_data; // opaque pointer back to driver instance
    netif_poll_fn poll;        // synchronously process pending RX (optional)

    // --- Stack-managed state (set by net::configure() or register_netif()) ---
    uint32_t     ipv4_addr;    // host byte order
    uint32_t     ipv4_netmask; // host byte order
    uint32_t     ipv4_gateway; // host byte order
    uint32_t     ipv4_dns;     // DNS server, host byte order (from DHCP)
    uint32_t     flags;        // NETIF_* flags
    bool         configured;

    // --- Internal linkage (managed by net subsystem) ---
    netif*       next;
};

/**
 * Initialize the network subsystem.
 * Must be called before drivers::init() so interfaces can register.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * Register a network interface with the stack.
 * The driver must have filled in name, mac, transmit, link_up, and driver_data.
 * The first interface registered becomes the default.
 * @note Safe to call from any kernel context.
 */
int32_t register_netif(netif* iface);

/**
 * Unregister a network interface (for hot-unplug).
 * @note Safe to call from any kernel context.
 */
int32_t unregister_netif(netif* iface);

/**
 * Configure IPv4 on a registered interface.
 * Addresses are in HOST byte order.
 */
int32_t configure(netif* iface, uint32_t ip, uint32_t netmask, uint32_t gateway);

/**
 * Get the default (first registered) network interface.
 * Returns nullptr if no interface is registered.
 */
netif* get_default_netif();

/**
 * Get the DNS server IP from the default interface.
 * Returns the address in HOST byte order, or 0 if none is configured.
 */
uint32_t get_dns_server();

/**
 * Find a network interface by name.
 * @return Pointer to the netif, or nullptr if not found.
 */
netif* find_netif(const char* name);

/**
 * Find a network interface by configured IPv4 address.
 * @param ip IPv4 address in host byte order.
 * @return Pointer to the netif, or nullptr if not found.
 */
netif* find_netif_by_ip(uint32_t ip);

/**
 * Check if an IPv4 address is configured on any local interface.
 * @param ip IPv4 address in host byte order.
 * @return true if the address is local.
 */
bool is_local_ip(uint32_t ip);

/**
 * Called by NIC drivers when a complete Ethernet frame is received.
 * The frame includes the Ethernet header. FCS should be stripped.
 * @param iface The interface that received the frame.
 * @param data  Pointer to the complete Ethernet frame.
 * @param len   Length of the frame in bytes.
 */
void rx_frame(netif* iface, const uint8_t* data, size_t len);

/**
 * Queue a protocol-generated response (e.g. ICMP echo reply) for
 * deferred transmission. Called from RX processing context where
 * inline TX would cause recursion through the ARP/poll path.
 * Packets are sent later by drain_deferred_tx().
 * @param iface    Interface to send on.
 * @param dst_ip   Destination IP in host byte order.
 * @param protocol IPv4 protocol number.
 * @param data     Payload data (copied into the queue).
 * @param len      Payload length.
 */
void queue_deferred_tx(netif* iface, uint32_t dst_ip, uint8_t protocol,
                       const uint8_t* data, size_t len);

/**
 * Queue a raw Ethernet frame for deferred transmission.
 * Used by ARP replies which bypass the IPv4 layer.
 */
void queue_deferred_eth_tx(netif* iface, const uint8_t* dst_mac,
                           uint16_t ethertype, const uint8_t* data, size_t len);

/**
 * Send all packets queued by queue_deferred_tx().
 * Called from the driver's run() or poll_callback after RX delivery
 * is complete. Runs at the top level, outside any RX processing,
 * so ipv4_send and ARP resolution are safe.
 */
void drain_deferred_tx();

} // namespace net

#endif // STELLUX_NET_NET_H
