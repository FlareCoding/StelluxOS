#include "net/ipv4.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/icmp.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/logging.h"
#include "common/string.h"

namespace net {

namespace {

__PRIVILEGED_DATA static volatile uint32_t g_ipv4_id_counter = 0;

static uint16_t next_ipv4_id() {
    return static_cast<uint16_t>(
        __atomic_fetch_add(&g_ipv4_id_counter, 1, __ATOMIC_RELAXED));
}

} // anonymous namespace

void ipv4_recv(netif* iface, const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(ipv4_header)) {
        return;
    }

    const auto* hdr = reinterpret_cast<const ipv4_header*>(data);

    // Check version
    uint8_t version = (hdr->ver_ihl >> 4) & 0xF;
    if (version != 4) return;

    // Get header length
    uint8_t ihl = hdr->ver_ihl & 0xF;
    if (ihl < 5) return;
    size_t header_len = static_cast<size_t>(ihl) * 4;
    if (header_len > len) return;

    // Verify total length
    uint16_t total_len = ntohs(hdr->total_len);
    if (total_len > len) return;
    if (total_len < header_len) return;

    // Verify header checksum
    uint16_t saved_cksum = hdr->checksum;
    // Compute checksum over the header with checksum field included
    uint16_t computed = inet_checksum(data, header_len);
    if (computed != 0) {
        log::debug("ipv4: bad header checksum, dropping");
        return;
    }

    // Check if packet is for us
    uint32_t dst_ip = ntohl(hdr->dst_ip);
    if (iface->configured && dst_ip != iface->ipv4_addr &&
        dst_ip != 0xFFFFFFFF &&
        dst_ip != (iface->ipv4_addr | ~iface->ipv4_netmask)) {
        return; // not for us
    }

    uint32_t src_ip = ntohl(hdr->src_ip);
    const uint8_t* payload = data + header_len;
    size_t payload_len = total_len - header_len;

    switch (hdr->protocol) {
    case IPV4_PROTO_ICMP:
        icmp_recv(iface, src_ip, payload, payload_len);
        break;
    default:
        break;
    }
}

int32_t ipv4_send(netif* iface, uint32_t dst_ip, uint8_t protocol,
                  const uint8_t* payload, size_t payload_len) {
    if (!iface || !iface->configured || !payload) {
        return ERR_INVAL;
    }

    if (payload_len > ETH_MTU - sizeof(ipv4_header)) {
        return ERR_INVAL; // too large (no fragmentation support)
    }

    // Build IPv4 header
    uint8_t packet[ETH_MTU];
    auto* hdr = reinterpret_cast<ipv4_header*>(packet);
    string::memset(hdr, 0, sizeof(ipv4_header));

    hdr->ver_ihl = (4 << 4) | 5; // IPv4, IHL=5 (20 bytes)
    hdr->tos = 0;
    hdr->total_len = htons(static_cast<uint16_t>(sizeof(ipv4_header) + payload_len));
    hdr->id = htons(next_ipv4_id());
    hdr->flags_frag = htons(0x4000); // Don't Fragment
    hdr->ttl = IPV4_DEFAULT_TTL;
    hdr->protocol = protocol;
    hdr->checksum = 0; // computed below
    hdr->src_ip = htonl(iface->ipv4_addr);
    hdr->dst_ip = htonl(dst_ip);

    // Compute header checksum
    hdr->checksum = inet_checksum(hdr, sizeof(ipv4_header));

    // Copy payload after header
    string::memcpy(packet + sizeof(ipv4_header), payload, payload_len);

    // Determine next-hop IP
    uint32_t next_hop = dst_ip;
    if ((dst_ip & iface->ipv4_netmask) != (iface->ipv4_addr & iface->ipv4_netmask)) {
        // Off-subnet — use gateway
        next_hop = iface->ipv4_gateway;
    }

    // Resolve MAC via ARP
    uint8_t dst_mac[MAC_ADDR_LEN];
    int32_t arp_rc = arp_resolve(iface, next_hop, dst_mac);
    if (arp_rc != OK) {
        return arp_rc;
    }

    // Send via Ethernet
    return eth_send(iface, dst_mac, ETH_TYPE_IPV4,
                    packet, sizeof(ipv4_header) + payload_len);
}

} // namespace net
