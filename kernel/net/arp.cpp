#include "net/arp.h"
#include "net/packet.h"
#include "net/interface.h"
#include "common/logging.h"

namespace net {
namespace arp {

static int32_t drop(interface* iface, packet* pkt, int32_t rc) {
    iface->record_packet_dropped();
    packet::free(pkt);
    return rc;
}

static int32_t reject(interface* iface, packet* pkt, int32_t rc) {
    iface->record_iface_error();
    packet::free(pkt);
    return rc;
}

int32_t input(packet* pkt) {
    if (!pkt) {
        log::warn("arp: input called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    if (!iface) {
        log::warn("arp: input called with a packet that has no interface");
        packet::free(pkt);
        return ERR_INVALID;
    }

    if (pkt->length() < HEADER_LEN) {
        return reject(iface, pkt, ERR_INVALID);
    }

    // Only Ethernet over IPv4 is supported
    arp_header* hdr = reinterpret_cast<arp_header*>(pkt->data());
    if (
        ntohs(hdr->hw_type) != HW_TYPE_ETHERNET ||
        ntohs(hdr->proto_type) != PROTO_TYPE_IPV4 ||
        hdr->hw_len != eth::MAC_ADDR_LEN ||
        hdr->proto_len != ipv4::ADDR_LEN
    ) {
        return drop(iface, pkt, OK);
    }

    // Requests for other hosts are normal traffic, not drops
    const ipv4::ipv4_config& conf = iface->ipv4_conf();
    if (!conf.configured() || hdr->target_proto_addr != conf.address) {
        packet::free(pkt);
        return OK;
    }

    if (ntohs(hdr->opcode) != OP_REQUEST) {
        return drop(iface, pkt, OK);
    }

    // RFC 826: the request becomes the reply in place, sender and target swapped
    hdr->target_hw_addr = hdr->sender_hw_addr;
    hdr->target_proto_addr = hdr->sender_proto_addr;
    hdr->sender_hw_addr = iface->mac();
    hdr->sender_proto_addr = conf.address;
    hdr->opcode = htons(OP_REPLY);

    // Strip link padding so it is not sent back as payload
    pkt->trim(HEADER_LEN);

    return output(pkt, hdr->target_hw_addr);
}

int32_t output(packet* pkt, const eth::mac_addr& dest) {
    return eth::output(pkt, dest, eth::TYPE_ARP);
}

void sweep(uint64_t) {
}

} // namespace arp
} // namespace net
