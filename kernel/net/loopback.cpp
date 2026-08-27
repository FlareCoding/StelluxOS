#include "net/loopback.h"
#include "net/net.h"
#include "net/ipv4.h"
#include "common/string.h"
#include "common/logging.h"

namespace net {

static netif g_lo_netif = {};
static bool g_lo_initialized = false;

/**
 * Loopback transmit callback. Feeds the frame straight back into rx_frame().
 * Safe from recursion because transmits only happen from top-level send
 * paths, never inside RX processing, replies use the deferred TX queue.
 */
static int32_t lo_transmit(netif* iface, const uint8_t* frame, size_t len) {
    if (!iface || !frame || len == 0) {
        return ERR_INVAL;
    }

    // Feed the frame back to the receive path.
    rx_frame(iface, frame, len);

    // Hardware NICs drain deferred TX from their driver event loop. Loopback
    // has none, so replies queued during this RX are drained right here.
    drain_deferred_tx();

    return OK;
}

/**
 * Loopback link status callback. Loopback is always up.
 */
static bool lo_link_up(netif*) {
    return true;
}

__PRIVILEGED_CODE int32_t loopback_init() {
    string::memset(&g_lo_netif, 0, sizeof(g_lo_netif));
    string::memcpy(g_lo_netif.name, "lo", 3);

    // Loopback has no real MAC. Ethernet framing still works because the
    // all-zeros MAC is never resolved via ARP.
    string::memset(g_lo_netif.mac, 0, MAC_ADDR_LEN);

    g_lo_netif.transmit    = lo_transmit;
    g_lo_netif.link_up     = lo_link_up;
    g_lo_netif.poll        = nullptr; // no polling needed
    g_lo_netif.driver_data = nullptr;
    g_lo_netif.flags       = NETIF_UP | NETIF_RUNNING | NETIF_LOOPBACK;

    int32_t rc = register_netif(&g_lo_netif);
    if (rc != OK) {
        log::error("loopback: failed to register interface");
        return rc;
    }

    // Configure with 127.0.0.1/8 (no gateway needed for loopback)
    rc = configure(&g_lo_netif,
                   ipv4_addr(127, 0, 0, 1),
                   ipv4_addr(255, 0, 0, 0),
                   0);
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
