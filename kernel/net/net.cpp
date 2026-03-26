#include "net/net.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/icmp.h"
#include "net/byteorder.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "common/logging.h"
#include "common/string.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

__PRIVILEGED_DATA static netif* g_iface_list = nullptr;
__PRIVILEGED_DATA static netif* g_default_iface = nullptr;
__PRIVILEGED_DATA static sync::spinlock g_net_lock = sync::SPINLOCK_INIT;

class net_device_node : public fs::node {
public:
    net_device_node(fs::instance* fs_inst, const char* name)
        : fs::node(fs::node_type::char_device, fs_inst, name) {
    }

    int32_t ioctl(fs::file* f, uint32_t cmd, uint64_t arg) override {
        (void)f;

        if (cmd == NET_PING) {
            return handle_ping(arg);
        }

        if (cmd == NET_GET_CONFIG) {
            return handle_get_config(arg);
        }

        return fs::ERR_NOSYS;
    }

    int32_t getattr(fs::vattr* attr) override {
        if (!attr) return fs::ERR_INVAL;
        attr->type = fs::node_type::char_device;
        attr->size = 0;
        return fs::OK;
    }

private:
    int32_t handle_ping(uint64_t arg) {
        if (arg == 0) return fs::ERR_INVAL;

        net_ping_req req = {};
        int32_t rc = mm::uaccess::copy_from_user(&req, reinterpret_cast<const void*>(arg), sizeof(req));
        if (rc != mm::uaccess::OK) return fs::ERR_INVAL;

        netif* iface = get_default_netif();
        if (!iface || !iface->configured) {
            req.result = ERR_NOIF;
            mm::uaccess::copy_to_user(reinterpret_cast<void*>(arg), &req, sizeof(req));
            return fs::OK;
        }

        uint32_t dst_ip = ntohl(req.dst_ip);
        uint32_t rtt_us = 0;

        int32_t ping_rc = icmp_ping(iface, dst_ip, req.id, req.seq,
                                    req.timeout_ms, &rtt_us);

        req.result = ping_rc;
        req.rtt_us = rtt_us;

        rc = mm::uaccess::copy_to_user(reinterpret_cast<void*>(arg), &req, sizeof(req));
        if (rc != mm::uaccess::OK) return fs::ERR_INVAL;

        return fs::OK;
    }

    int32_t handle_get_config(uint64_t arg) {
        if (arg == 0) return fs::ERR_INVAL;

        netif* iface = get_default_netif();
        net_config_info info = {};

        if (iface) {
            string::memcpy(info.mac, iface->mac, MAC_ADDR_LEN);
            info.ipv4_addr = htonl(iface->ipv4_addr);
            info.ipv4_netmask = htonl(iface->ipv4_netmask);
            info.ipv4_gateway = htonl(iface->ipv4_gateway);
            string::memcpy(info.name, iface->name, sizeof(info.name) - 1);
        }

        int32_t rc = mm::uaccess::copy_to_user(reinterpret_cast<void*>(arg), &info, sizeof(info));
        if (rc != mm::uaccess::OK) return fs::ERR_INVAL;

        return fs::OK;
    }
};

} // anonymous namespace

__PRIVILEGED_CODE int32_t init() {
    // Initialize protocol subsystems
    arp_init();
    icmp_init();

    // Register /dev/net0
    void* mem = heap::kzalloc(sizeof(net_device_node));
    if (!mem) {
        log::error("net: failed to allocate net_device_node");
        return ERR_NOMEM;
    }

    auto* dev_node = new (mem) net_device_node(nullptr, "net0");

    int32_t rc = devfs::add_char_device("net0", dev_node);
    if (rc != devfs::OK) {
        log::error("net: failed to register /dev/net0");
        dev_node->~net_device_node();
        heap::kfree(mem);
        return ERR_INIT;
    }

    log::info("net: initialized, /dev/net0 registered");
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

    // Remove from list
    netif** pp = &g_iface_list;
    while (*pp) {
        if (*pp == iface) {
            *pp = iface->next;
            break;
        }
        pp = &(*pp)->next;
    }

    if (g_default_iface == iface) {
        g_default_iface = g_iface_list; // may be nullptr
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
