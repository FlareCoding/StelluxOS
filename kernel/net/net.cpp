#include "net/net.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/arp.h"
#include "common/logging.h"
#include "common/string.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

__PRIVILEGED_DATA static netif* g_iface_list = nullptr;
__PRIVILEGED_DATA static netif* g_default_iface = nullptr;
__PRIVILEGED_DATA static sync::spinlock g_net_lock = sync::SPINLOCK_INIT;

// Deferred TX queue: protocol-generated responses (e.g. ICMP echo replies)
// that cannot be sent inline from RX processing context.
constexpr uint32_t DEFERRED_TX_MAX = 8;

enum class deferred_tx_kind : uint8_t {
    ipv4,     // send via ipv4_send (dst_ip + protocol + payload)
    ethernet, // send via eth_send (dst_mac + ethertype + payload)
};

struct deferred_tx_entry {
    netif*           iface;
    deferred_tx_kind kind;
    size_t           len;
    uint8_t          data[ETH_MTU];

    // IPv4-level fields
    uint32_t dst_ip;
    uint8_t  protocol;

    // Ethernet-level fields
    uint8_t  dst_mac[MAC_ADDR_LEN];
    uint16_t ethertype;
};

__PRIVILEGED_DATA static deferred_tx_entry g_deferred_tx[DEFERRED_TX_MAX] = {};
__PRIVILEGED_DATA static uint32_t g_deferred_tx_count = 0;
__PRIVILEGED_DATA static sync::spinlock g_deferred_tx_lock = sync::SPINLOCK_INIT;

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

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_net_lock);
        iface->next = g_iface_list;
        g_iface_list = iface;
        if (!g_default_iface) {
            g_default_iface = iface;
        }
    });

    log::info("net: registered interface %s (%02x:%02x:%02x:%02x:%02x:%02x)",
              iface->name,
              iface->mac[0], iface->mac[1], iface->mac[2],
              iface->mac[3], iface->mac[4], iface->mac[5]);
    return OK;
}

int32_t unregister_netif(netif* iface) {
    if (!iface) return ERR_INVAL;

    RUN_ELEVATED({
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
    });

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

void queue_deferred_tx(netif* iface, uint32_t dst_ip, uint8_t protocol,
                       const uint8_t* data, size_t len) {
    if (!iface || !data || len == 0 || len > ETH_MTU) {
        return;
    }

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_deferred_tx_lock);

        if (g_deferred_tx_count < DEFERRED_TX_MAX) {
            auto& entry = g_deferred_tx[g_deferred_tx_count];
            entry.iface = iface;
            entry.kind = deferred_tx_kind::ipv4;
            entry.dst_ip = dst_ip;
            entry.protocol = protocol;
            entry.len = len;
            string::memcpy(entry.data, data, len);
            g_deferred_tx_count++;
        }
    });
}

void queue_deferred_eth_tx(netif* iface, const uint8_t* dst_mac,
                           uint16_t ethertype, const uint8_t* data, size_t len) {
    if (!iface || !dst_mac || !data || len == 0 || len > ETH_MTU) {
        return;
    }

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_deferred_tx_lock);

        if (g_deferred_tx_count < DEFERRED_TX_MAX) {
            auto& entry = g_deferred_tx[g_deferred_tx_count];
            entry.iface = iface;
            entry.kind = deferred_tx_kind::ethernet;
            string::memcpy(entry.dst_mac, dst_mac, MAC_ADDR_LEN);
            entry.ethertype = ethertype;
            entry.len = len;
            string::memcpy(entry.data, data, len);
            g_deferred_tx_count++;
        }
    });
}

void drain_deferred_tx() {
    // Snapshot and clear the queue under the lock, then send outside it.
    uint32_t count = 0;

    // Heap-allocate the snapshot to avoid putting ~12KB on the kernel stack.
    auto* local = static_cast<deferred_tx_entry*>(
        heap::kzalloc(DEFERRED_TX_MAX * sizeof(deferred_tx_entry)));
    if (!local) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_deferred_tx_lock);
        count = g_deferred_tx_count;
        if (count > 0) {
            string::memcpy(local, g_deferred_tx, count * sizeof(deferred_tx_entry));
            g_deferred_tx_count = 0;
        }
    });

    for (uint32_t i = 0; i < count; i++) {
        if (local[i].kind == deferred_tx_kind::ipv4) {
            ipv4_send(local[i].iface, local[i].dst_ip, local[i].protocol,
                      local[i].data, local[i].len);
        } else if (local[i].kind == deferred_tx_kind::ethernet) {
            eth_send(local[i].iface, local[i].dst_mac, local[i].ethertype,
                     local[i].data, local[i].len);
        }
    }

    heap::kfree(local);
}

} // namespace net
