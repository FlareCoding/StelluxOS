#include "net/arp.h"
#include "net/ethernet.h"
#include "net/byteorder.h"
#include "common/logging.h"
#include "common/string.h"
#include "sync/spinlock.h"
#include "hw/cpu.h"
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

struct arp_entry {
    uint32_t ip;                // host byte order
    uint8_t  mac[MAC_ADDR_LEN];
    bool     valid;
};

__PRIVILEGED_DATA static arp_entry g_arp_table[ARP_TABLE_SIZE] = {};
__PRIVILEGED_DATA static sync::spinlock g_arp_lock = sync::SPINLOCK_INIT;

} // anonymous namespace

void arp_init() {
    for (uint32_t i = 0; i < ARP_TABLE_SIZE; i++) {
        g_arp_table[i].valid = false;
    }
}

static void arp_table_update(uint32_t ip, const uint8_t* mac) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_arp_lock);

        bool updated = false;
        for (uint32_t i = 0; i < ARP_TABLE_SIZE; i++) {
            if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
                string::memcpy(g_arp_table[i].mac, mac, MAC_ADDR_LEN);
                updated = true;
                break;
            }
        }

        if (!updated) {
            for (uint32_t i = 0; i < ARP_TABLE_SIZE; i++) {
                if (!g_arp_table[i].valid) {
                    g_arp_table[i].ip = ip;
                    string::memcpy(g_arp_table[i].mac, mac, MAC_ADDR_LEN);
                    g_arp_table[i].valid = true;
                    updated = true;
                    break;
                }
            }
        }

        if (!updated) {
            g_arp_table[0].ip = ip;
            string::memcpy(g_arp_table[0].mac, mac, MAC_ADDR_LEN);
            g_arp_table[0].valid = true;
        }

        sync::spin_unlock_irqrestore(g_arp_lock, irq);
    });
}

static bool arp_table_lookup(uint32_t ip, uint8_t* out_mac) {
    bool found = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_arp_lock);
        for (uint32_t i = 0; i < ARP_TABLE_SIZE; i++) {
            if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
                string::memcpy(out_mac, g_arp_table[i].mac, MAC_ADDR_LEN);
                found = true;
                break;
            }
        }
    });
    return found;
}

void arp_recv(netif* iface, const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(arp_header)) {
        return;
    }

    const auto* arp = reinterpret_cast<const arp_header*>(data);

    if (ntohs(arp->hw_type) != ARP_HW_ETHERNET) return;
    if (ntohs(arp->proto_type) != ETH_TYPE_IPV4) return;
    if (arp->hw_len != MAC_ADDR_LEN) return;
    if (arp->proto_len != 4) return;

    uint32_t sender_ip = ntohl(arp->sender_ip);
    uint32_t target_ip = ntohl(arp->target_ip);
    uint16_t opcode = ntohs(arp->opcode);

    // Always learn from incoming ARP packets
    arp_table_update(sender_ip, arp->sender_mac);

    if (opcode == ARP_OP_REQUEST && iface->configured && target_ip == iface->ipv4_addr) {
        // Send ARP reply
        log::debug("arp: replying to request from %u.%u.%u.%u",
                   (sender_ip >> 24) & 0xFF, (sender_ip >> 16) & 0xFF,
                   (sender_ip >> 8) & 0xFF, sender_ip & 0xFF);

        arp_header reply = {};
        reply.hw_type = htons(ARP_HW_ETHERNET);
        reply.proto_type = htons(ETH_TYPE_IPV4);
        reply.hw_len = MAC_ADDR_LEN;
        reply.proto_len = 4;
        reply.opcode = htons(ARP_OP_REPLY);
        string::memcpy(reply.sender_mac, iface->mac, MAC_ADDR_LEN);
        reply.sender_ip = htonl(iface->ipv4_addr);
        string::memcpy(reply.target_mac, arp->sender_mac, MAC_ADDR_LEN);
        reply.target_ip = arp->sender_ip;

        eth_send(iface, arp->sender_mac, ETH_TYPE_ARP,
                 reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
    } else if (opcode == ARP_OP_REPLY) {
        log::debug("arp: got reply from %u.%u.%u.%u",
                   (sender_ip >> 24) & 0xFF, (sender_ip >> 16) & 0xFF,
                   (sender_ip >> 8) & 0xFF, sender_ip & 0xFF);
    }
}

void arp_send_request(netif* iface, uint32_t target_ip) {
    if (!iface || !iface->configured) return;

    arp_header req = {};
    req.hw_type = htons(ARP_HW_ETHERNET);
    req.proto_type = htons(ETH_TYPE_IPV4);
    req.hw_len = MAC_ADDR_LEN;
    req.proto_len = 4;
    req.opcode = htons(ARP_OP_REQUEST);
    string::memcpy(req.sender_mac, iface->mac, MAC_ADDR_LEN);
    req.sender_ip = htonl(iface->ipv4_addr);
    string::memset(req.target_mac, 0, MAC_ADDR_LEN);
    req.target_ip = htonl(target_ip);

    eth_send(iface, ETH_BROADCAST, ETH_TYPE_ARP,
             reinterpret_cast<const uint8_t*>(&req), sizeof(req));
}

int32_t arp_resolve(netif* iface, uint32_t target_ip, uint8_t* out_mac) {
    if (!iface || !out_mac) return ERR_INVAL;

    // Check if it's a broadcast address
    if (target_ip == 0xFFFFFFFF ||
        target_ip == (iface->ipv4_addr | ~iface->ipv4_netmask)) {
        string::memcpy(out_mac, ETH_BROADCAST, MAC_ADDR_LEN);
        return OK;
    }

    // Check cache first
    if (arp_table_lookup(target_ip, out_mac)) {
        return OK;
    }

    // Poll the driver's RX path to process incoming packets synchronously.
    for (uint32_t attempt = 0; attempt < ARP_RETRY_COUNT; attempt++) {
        arp_send_request(iface, target_ip);

        for (uint32_t poll = 0; poll < 5000; poll++) {
            RUN_ELEVATED({
                if (iface->poll) {
                    iface->poll(iface);
                }
            });

            if (arp_table_lookup(target_ip, out_mac)) {
                return OK;
            }

            for (uint32_t j = 0; j < 100; j++) {
                cpu::relax();
            }
        }
    }

    log::warn("arp: failed to resolve %u.%u.%u.%u",
              (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
              (target_ip >> 8) & 0xFF, target_ip & 0xFF);
    return ERR_NOARP;
}

} // namespace net
