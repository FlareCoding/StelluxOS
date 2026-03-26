#include "net/net.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "common/logging.h"
#include "common/string.h"
#include "sync/spinlock.h"

namespace net {

namespace {

__PRIVILEGED_DATA static netif* g_iface_list = nullptr;
__PRIVILEGED_DATA static netif* g_default_iface = nullptr;
__PRIVILEGED_DATA static sync::spinlock g_net_lock = sync::SPINLOCK_INIT;

} // anonymous namespace

__PRIVILEGED_CODE int32_t init() {
    arp_init();
    log::info("net: initialized");
    return OK;
}

int32_t register_netif(netif* iface) {
    if (!iface || !iface->transmit) {
        return ERR_INVAL;
    }

    iface->configured = false;
    iface->next = nullptr;

    {
        sync::irq_lock_guard guard(g_net_lock);
        iface->next = g_iface_list;
        g_iface_list = iface;
        if (!g_default_iface) {
            g_default_iface = iface;
        }
    }

    log::info("net: registered interface %s (%02x:%02x:%02x:%02x:%02x:%02x)",
              iface->name,
              iface->mac[0], iface->mac[1], iface->mac[2],
              iface->mac[3], iface->mac[4], iface->mac[5]);
    return OK;
}

int32_t unregister_netif(netif* iface) {
    if (!iface) return ERR_INVAL;

    sync::irq_lock_guard guard(g_net_lock);

    netif** pp = &g_iface_list;
    while (*pp) {
        if (*pp == iface) {
            *pp = iface->next;
            break;
        }
        pp = &(*pp)->next;
    }

    if (g_default_iface == iface) {
        g_default_iface = g_iface_list;
    }

    iface->next = nullptr;
    return OK;
}

int32_t configure(netif* iface, uint32_t ip, uint32_t netmask, uint32_t gateway) {
    if (!iface) return ERR_INVAL;

    iface->ipv4_addr = ip;
    iface->ipv4_netmask = netmask;
    iface->ipv4_gateway = gateway;
    iface->configured = true;

    log::info("net: %s configured %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u",
              iface->name,
              (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
              (ip >> 8) & 0xFF, ip & 0xFF,
              (netmask >> 24) & 0xFF, (netmask >> 16) & 0xFF,
              (netmask >> 8) & 0xFF, netmask & 0xFF,
              (gateway >> 24) & 0xFF, (gateway >> 16) & 0xFF,
              (gateway >> 8) & 0xFF, gateway & 0xFF);
    return OK;
}

netif* get_default_netif() {
    return g_default_iface;
}

void rx_frame(netif* iface, const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(eth_header)) {
        return;
    }
    eth_recv(iface, data, len);
}

} // namespace net
