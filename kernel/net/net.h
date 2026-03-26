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

// Ioctl commands for /dev/net0
constexpr uint32_t NET_PING       = 0x4E01;
constexpr uint32_t NET_GET_CONFIG = 0x4E02;

// Maximum Ethernet frame size (without FCS)
constexpr size_t ETH_FRAME_MAX = 1514;
constexpr size_t ETH_MTU       = 1500;
constexpr size_t MAC_ADDR_LEN  = 6;
constexpr size_t MAX_INTERFACES = 8;

// Ping request structure (userland ↔ kernel ioctl interface)
struct net_ping_req {
    uint32_t dst_ip;       // in: target IP (network byte order)
    uint16_t id;           // in: ICMP identifier
    uint16_t seq;          // in: ICMP sequence number
    uint32_t timeout_ms;   // in: timeout in milliseconds
    int32_t  result;       // out: 0=success, negative=error
    uint32_t rtt_us;       // out: round-trip time in microseconds
} __attribute__((packed));

// Network config info structure (userland ↔ kernel ioctl)
struct net_config_info {
    uint8_t  mac[MAC_ADDR_LEN];
    uint8_t  padding[2];
    uint32_t ipv4_addr;    // network byte order
    uint32_t ipv4_netmask; // network byte order
    uint32_t ipv4_gateway; // network byte order
    char     name[16];
} __attribute__((packed));

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

    // --- Stack-managed state (set by net::configure()) ---
    uint32_t     ipv4_addr;    // host byte order
    uint32_t     ipv4_netmask; // host byte order
    uint32_t     ipv4_gateway; // host byte order
    bool         configured;

    // --- Internal linkage (managed by net subsystem) ---
    netif*       next;
};

/**
 * Initialize the network subsystem.
 * Registers /dev/net0 in devfs.
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
 * Called by NIC drivers when a complete Ethernet frame is received.
 * The frame includes the Ethernet header. FCS should be stripped.
 * @param iface The interface that received the frame.
 * @param data  Pointer to the complete Ethernet frame.
 * @param len   Length of the frame in bytes.
 */
void rx_frame(netif* iface, const uint8_t* data, size_t len);


} // namespace net

#endif // STELLUX_NET_NET_H
