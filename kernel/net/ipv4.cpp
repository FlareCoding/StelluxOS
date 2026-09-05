#include "net/ipv4.h"
#include "net/interface.h"
#include "net/checksum.h"
#include "common/logging.h"

namespace net {
namespace ipv4 {

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

bool ipv4_config::is_subnet_broadcast(const ipv4_addr& addr) const {
    if (!configured() || !addr.in_same_subnet(address, netmask)) {
        return false;
    }

    for (size_t i = 0; i < ADDR_LEN; i++) {
        uint8_t host_bits = static_cast<uint8_t>(~netmask.bytes[i]);
        if ((addr.bytes[i] & host_bits) != host_bits) {
            return false;
        }
    }

    return true;
}

int32_t input(packet* pkt) {
    if (!pkt) {
        log::warn("ipv4: input called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    if (!iface) {
        log::warn("ipv4: input called with a packet that has no interface");
        packet::free(pkt);
        return ERR_INVALID;
    }

    if (pkt->length() < HEADER_LEN) {
        return reject(iface, pkt, ERR_INVALID);
    }

    const ipv4_header* hdr = reinterpret_cast<const ipv4_header*>(pkt->data());
    if (hdr->version() != VERSION || hdr->ihl() < MIN_IHL || hdr->header_len() > pkt->length()) {
        return reject(iface, pkt, ERR_INVALID);
    }

    if (checksum(hdr, hdr->header_len()) != 0) {
        return reject(iface, pkt, ERR_INVALID);
    }

    // The header's own length is authoritative, anything past it is link padding
    size_t total_len = ntohs(hdr->total_len);
    if (total_len < hdr->header_len() || total_len > pkt->length()) {
        return reject(iface, pkt, ERR_INVALID);
    }

    pkt->trim(total_len);

    // Weak host model: any address this host owns is accepted on any interface,
    // which is what local delivery through loopback needs.
    bool for_local_host = find_interface_by_address(hdr->dst) != nullptr ||
                          hdr->dst.is_broadcast() ||
                          iface->ipv4_conf().is_subnet_broadcast(hdr->dst);

    if (!for_local_host) {
        packet::free(pkt);
        return OK;
    }

    // Fragments are not reassembled
    if (hdr->frag_off() != 0 || (hdr->flags() & FLAG_MF) != 0) {
        return drop(iface, pkt, OK);
    }

    pkt->mark_network_header();
    (void)pkt->pull(hdr->header_len());

    const uint8_t* src = hdr->src.bytes;
    switch (hdr->proto) {
    case PROTO_ICMP:
        log::info("ipv4: ICMP datagram from %u.%u.%u.%u, %lu payload bytes",
                  src[0], src[1], src[2], src[3], pkt->length());
        packet::free(pkt);
        break;
    case PROTO_UDP:
        log::info("ipv4: UDP datagram from %u.%u.%u.%u, %lu payload bytes",
                  src[0], src[1], src[2], src[3], pkt->length());
        packet::free(pkt);
        break;
    case PROTO_TCP:
        log::info("ipv4: TCP datagram from %u.%u.%u.%u, %lu payload bytes",
                  src[0], src[1], src[2], src[3], pkt->length());
        packet::free(pkt);
        break;
    default:
        return drop(iface, pkt, OK);
    }

    return OK;
}

int32_t output(packet* pkt, const ipv4_addr& dest, uint8_t protocol) {
    (void)dest;
    (void)protocol;
    packet::free(pkt);
    return ERR_INVALID;
}

} // namespace ipv4
} // namespace net
