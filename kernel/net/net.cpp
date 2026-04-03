#include "net/net.h"
#include "net/netinfo.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/arp.h"
#include "net/loopback.h"
#include "net/route.h"
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
    route_init();

    int32_t lo_rc = loopback_init();
    if (lo_rc != OK) {
        log::warn("net: loopback init failed");
    }

    log::info("net: initialized");
    return OK;
}

int32_t register_netif(netif* iface) {
    if (!iface || !iface->transmit) {
        return ERR_INVAL;
    }

    iface->configured = false;
    iface->next = nullptr;

    // Mark as administratively up on registration (unless already set
    // with specific flags, e.g. loopback sets flags before registration).
    if (!(iface->flags & NETIF_UP)) {
        iface->flags |= NETIF_UP;
    }

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_net_lock);
        iface->next = g_iface_list;
        g_iface_list = iface;
        // Set as default only if no default exists AND this is not loopback.
        // Loopback should never be the default outbound interface — external
        // traffic must go through a real NIC. If loopback is the only
        // interface, g_default_iface stays nullptr until a real NIC registers.
        if (!g_default_iface && !(iface->flags & NETIF_LOOPBACK)) {
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

    // Clean up all routing table entries owned by this interface BEFORE
    // removing from the interface list. route_del_iface matches on the
    // owner field, so LOCAL routes (which point to loopback but are owned
    // by this interface) are correctly removed without affecting other
    // interfaces' LOCAL routes.
    route_del_iface(iface);

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

        // Re-evaluate the default interface: pick the first non-loopback
        // configured interface, or nullptr if none exists.
        if (g_default_iface == iface) {
            g_default_iface = nullptr;
            for (netif* cur = g_iface_list; cur; cur = cur->next) {
                if (!(cur->flags & NETIF_LOOPBACK) && cur->configured) {
                    g_default_iface = cur;
                    break;
                }
            }
        }
    });

    iface->next = nullptr;
    return OK;
}

int32_t configure(netif* iface, uint32_t ip, uint32_t netmask, uint32_t gateway) {
    if (!iface) return ERR_INVAL;

    // Clear any existing routes owned by this interface (handles reconfiguration).
    // route_del_iface matches on the owner field, so LOCAL routes (which point
    // to loopback) are correctly cleaned up without affecting other interfaces'
    // LOCAL routes.
    route_del_iface(iface);

    iface->ipv4_addr = ip;
    iface->ipv4_netmask = netmask;
    iface->ipv4_gateway = gateway;
    iface->configured = true;

    // Auto-populate routes for this interface
    route_add_interface_routes(iface);

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

uint32_t get_dns_server() {
    netif* iface = g_default_iface;
    if (iface && iface->configured) {
        return iface->ipv4_dns;
    }
    return 0;
}

netif* find_netif(const char* name) {
    if (!name) return nullptr;

    netif* result = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_net_lock);
        for (netif* cur = g_iface_list; cur; cur = cur->next) {
            if (string::strcmp(cur->name, name) == 0) {
                result = cur;
                break;
            }
        }
    });
    return result;
}

netif* find_netif_by_ip(uint32_t ip) {
    if (ip == 0) return nullptr;

    netif* result = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_net_lock);
        for (netif* cur = g_iface_list; cur; cur = cur->next) {
            if (cur->configured && cur->ipv4_addr == ip) {
                result = cur;
                break;
            }
        }
    });
    return result;
}

bool is_local_ip(uint32_t ip) {
    return find_netif_by_ip(ip) != nullptr;
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

    // Heap-allocate the snapshot
    auto* local = static_cast<deferred_tx_entry*>(
        heap::kzalloc(DEFERRED_TX_MAX * sizeof(deferred_tx_entry))
    );
    if (!local) {
        return;
    }

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

__PRIVILEGED_CODE int32_t query_status(net_status* out) {
    if (!out) return ERR_INVAL;

    string::memset(out, 0, sizeof(net_status));

    // Snapshot interface data under the lock.
    // Save link_up callback + netif pointer for each interface so we
    // can query live link status outside the lock (avoids calling
    // driver callbacks while holding g_net_lock).
    struct snapshot_entry {
        netif* iface;
        netif_link_fn link_fn;
        bool is_default;
    };
    snapshot_entry snap[MAX_INTERFACES];
    uint32_t count = 0;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_net_lock);

        netif* cur = g_iface_list;
        while (cur && count < MAX_INTERFACES) {
            auto& e = out->interfaces[count];
            string::memcpy(e.name, cur->name, 16);
            string::memcpy(e.mac, cur->mac, MAC_ADDR_LEN);
            e.ipv4_addr    = cur->ipv4_addr;
            e.ipv4_netmask = cur->ipv4_netmask;
            e.ipv4_gateway = cur->ipv4_gateway;
            e.ipv4_dns     = cur->ipv4_dns;
            e.flags = 0;
            if (cur->configured) {
                e.flags |= IFF_CONFIGURED;
            }
            if (cur->flags & NETIF_LOOPBACK) {
                e.flags |= IFF_LOOPBACK;
            }

            snap[count].iface = cur;
            snap[count].link_fn = cur->link_up;
            snap[count].is_default = (cur == g_default_iface);
            count++;
            cur = cur->next;
        }
    });

    // Query live link status outside the lock.
    for (uint32_t i = 0; i < count; i++) {
        if (snap[i].link_fn && snap[i].link_fn(snap[i].iface)) {
            out->interfaces[i].flags |= IFF_UP;
        }
        if (snap[i].is_default) {
            out->interfaces[i].flags |= IFF_DEFAULT;
        }
    }

    out->if_count = count;
    return OK;
}

} // namespace net
