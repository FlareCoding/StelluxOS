#include "net/loopback.h"
#include "net/net.h"
#include "net/ipv4.h"
#include "common/string.h"
#include "common/logging.h"

namespace net {

namespace {

static netif g_lo_netif = {};
static bool g_lo_initialized = false;

/**
 * Loopback transmit callback.
 * Feeds the frame directly back to the receive path.
 *
 * Safety: This is called from eth_send() which is called from:
 *   (a) ipv4_send() — top-level send path, not inside RX processing
 *   (b) drain_deferred_tx() — also top-level, after RX delivery
 * In both cases, we are NOT inside rx_frame() processing, so calling
 * rx_frame() here does not cause recursion. Protocol handlers that
 * need to reply (e.g. ICMP echo) use queue_deferred_tx() to defer
 * their responses.
 */
static int32_t lo_transmit(netif* iface, const uint8_t* frame, size_t len) {
    if (!iface || !frame || len == 0) {
        return ERR_INVAL;
    }

    // Feed the frame back to the receive path.
    rx_frame(iface, frame, len);

    // Drain any deferred TX that was queued during RX processing
    // (e.g. ICMP echo replies). For hardware NICs, this is done by
    // the driver's run() loop or poll_callback(). Since loopback has
    // no driver event loop, we drain immediately here.
    drain_deferred_tx();

    return OK;
}

/**
 * Loopback link status callback. Loopback is always up.
 */
static bool lo_link_up(netif*) {
    return true;
}

} // anonymous namespace

__PRIVILEGED_CODE int32_t loopback_init() {
    // Initialize the loopback netif
    string::memset(&g_lo_netif, 0, sizeof(g_lo_netif));
    string::memcpy(g_lo_netif.name, "lo", 3);

    // Loopback has no real MAC address — use all zeros.
    // Ethernet framing still works; the MAC is never resolved via ARP.
    string::memset(g_lo_netif.mac, 0, MAC_ADDR_LEN);

    g_lo_netif.transmit    = lo_transmit;
    g_lo_netif.link_up     = lo_link_up;
    g_lo_netif.poll        = nullptr;  // no polling needed
    g_lo_netif.driver_data = nullptr;
    g_lo_netif.flags       = NETIF_UP | NETIF_RUNNING | NETIF_LOOPBACK;

    // Register with the network stack.
    int32_t rc = register_netif(&g_lo_netif);
    if (rc != OK) {
        log::error("loopback: failed to register interface");
        return rc;
    }

    // Configure with 127.0.0.1/8 (no gateway needed for loopback)
    rc = configure(&g_lo_netif,
                   ipv4_addr(127, 0, 0, 1),    // IP
                   ipv4_addr(255, 0, 0, 0),     // Netmask (/8)
                   0);                           // No gateway
    if (rc != OK) {
        log::error("loopback: failed to configure interface");
        return rc;
    }

    g_lo_initialized = true;
    log::info("loopback: initialized lo (127.0.0.1/8)");
    return OK;
}

netif* get_loopback_netif() {
    return g_lo_initialized ? &g_lo_netif : nullptr;
}

} // namespace net
