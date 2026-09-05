#include "net/eth.h"
#include "net/packet.h"
#include "net/interface.h"
#include "common/logging.h"

namespace net {
namespace eth {

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

static void log_frame(const char* proto, const eth_header* hdr, size_t payload_len) {
    const uint8_t* src = hdr->src.bytes;
    log::info("eth: %s frame from %02x:%02x:%02x:%02x:%02x:%02x, %lu payload bytes",
              proto, src[0], src[1], src[2], src[3], src[4], src[5], payload_len);
}

int32_t input(packet* pkt) {
    if (!pkt) {
        log::warn("eth: input called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    if (!iface) {
        log::warn("eth: input called with a packet that has no interface");
        packet::free(pkt);
        return ERR_INVALID;
    }

    // A frame shorter than its own header is damaged
    if (pkt->length() < HEADER_LEN) {
        return reject(iface, pkt, ERR_INVALID);
    }

    pkt->mark_link_header();
    const eth_header* hdr = reinterpret_cast<const eth_header*>(pkt->link_header());

    (void)pkt->pull(HEADER_LEN);

    // The NIC already filters to our address plus multicast, this is defense
    // against a NIC in promiscuous mode or a driver that skipped its filter
    if (hdr->dest != iface->mac() && !hdr->dest.is_multicast()) {
        return drop(iface, pkt, OK);
    }

    // Below TYPE_MIN the field is an IEEE 802.3 length, not a type
    uint16_t type = ntohs(hdr->type);
    if (type < TYPE_MIN) {
        return drop(iface, pkt, OK);
    }

    switch (type) {
    case TYPE_ARP:
        log_frame("ARP", hdr, pkt->length());
        packet::free(pkt);
        break;
    case TYPE_IPV4:
        log_frame("IPv4", hdr, pkt->length());
        packet::free(pkt);
        break;
    default:
        return drop(iface, pkt, OK);
    }

    return OK;
}

int32_t output(packet* pkt, const mac_addr& dest, uint16_t type) {
    if (!pkt) {
        log::warn("eth: output called with no packet");
        return ERR_INVALID;
    }

    // Routing chooses the interface and records it on the packet before
    // handing it down, so a missing one is a bug in the layer above.
    interface* iface = pkt->iface();
    if (!iface) {
        log::warn("eth: output called with a packet that has no interface");
        packet::free(pkt);
        return ERR_INVALID;
    }

    if (!iface->enabled()) {
        return drop(iface, pkt, ERR_DOWN);
    }

    // The window holds the payload at this point, and the MTU bounds exactly that
    if (pkt->length() > iface->mtu()) {
        return drop(iface, pkt, ERR_TOO_LARGE);
    }

    // No headroom means the packet was allocated without reserving room for
    // the headers below it, which is a bug in the layer that allocated it.
    eth_header* hdr = reinterpret_cast<eth_header*>(pkt->push(HEADER_LEN));
    if (!hdr) {
        log::warn("eth: output packet has no headroom for the header");
        return reject(iface, pkt, ERR_INVALID);
    }

    hdr->dest = dest;
    hdr->src = iface->mac();
    hdr->type = htons(type);
    pkt->mark_link_header();

    // The interface borrows the packet and copies the frame out,
    // so the packet is released here on every outcome.
    int32_t rc = iface->transmit(pkt);
    packet::free(pkt);

    return rc;
}

} // namespace eth
} // namespace net
