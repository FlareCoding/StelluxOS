#include "net/ipv4.h"
#include "net/interface.h"
#include "net/checksum.h"
#include "net/route.h"
#include "net/arp.h"
#include "net/eth.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "sync/atomic.h"
#include "common/logging.h"

namespace net {
namespace ipv4 {

// Identification field of the next datagram sent, only meaningful to reassembly
static sync::atomic<uint16_t> g_next_id {0};

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

    // The all-ones host number is the broadcast, the all-zeros one its obsolete
    // form that hosts still accept (RFC 1122 3.3.6), a /31 or /32 has neither
    int host_bits = 0;
    bool all_ones = true;
    bool all_zeros = true;

    for (size_t i = 0; i < ADDR_LEN; i++) {
        uint8_t host_mask = static_cast<uint8_t>(~netmask.bytes[i]);
        host_bits += __builtin_popcount(host_mask);

        all_ones = all_ones && (addr.bytes[i] & host_mask) == host_mask;
        all_zeros = all_zeros && (addr.bytes[i] & host_mask) == 0;
    }

    return host_bits >= 2 && (all_ones || all_zeros);
}

bool ipv4_config::is_unicast(const ipv4_addr& addr) const {
    if (addr.in_zero_network() || addr.is_multicast() || addr.is_reserved() ||
        is_subnet_broadcast(addr)) {
        return false;
    }

    return addr.is_loopback() ? address.is_loopback() : true;
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

    // Weak host model: any address this host owns is accepted on any interface. The
    // loopback network is the exception, it never arrives from a link (RFC 1122 3.2.1.3).
    ipv4_config conf = iface->ipv4_conf();
    bool for_local_host;

    if (hdr->dst.is_loopback()) {
        for_local_host = iface->is_loopback();
    } else {
        for_local_host = find_interface_by_address(hdr->dst) != nullptr ||
                         hdr->dst.is_broadcast() ||
                         conf.is_subnet_broadcast(hdr->dst);
    }

    if (!for_local_host) {
        packet::free(pkt);
        return OK;
    }

    // RFC 1122 3.2.1.3: the source must name a single host. A host still acquiring
    // its address may send from the zero network to the limited broadcast.
    bool acquiring = hdr->src.in_zero_network() && hdr->dst.is_broadcast();

    if (!acquiring && !conf.is_unicast(hdr->src)) {
        return reject(iface, pkt, ERR_INVALID);
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
        return icmp::input(pkt);
    case PROTO_UDP:
        return udp::input(pkt);
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
    if (!pkt) {
        log::warn("ipv4: output called with no packet");
        return ERR_INVALID;
    }

    route::route_result route;
    int32_t rc = route::lookup(dest, &route);
    if (rc != OK) {
        packet::free(pkt);
        return rc;
    }

    // Nothing is fragmented, so the payload must fit one frame behind the header
    if (pkt->length() > static_cast<size_t>(route.iface->mtu()) - HEADER_LEN) {
        return drop(route.iface, pkt, ERR_TOO_LARGE);
    }

    ipv4_header* hdr = reinterpret_cast<ipv4_header*>(pkt->push(HEADER_LEN));
    if (!hdr) {
        log::warn("ipv4: output packet has no headroom for the header");
        return reject(route.iface, pkt, ERR_INVALID);
    }

    hdr->set_version_ihl(VERSION, MIN_IHL);
    hdr->tos = 0;
    hdr->total_len = htons(static_cast<uint16_t>(pkt->length()));
    hdr->id = htons(g_next_id.fetch_add_relaxed(1));
    hdr->fl_frag_off = htons(FLAG_DF);
    hdr->ttl = DEFAULT_TTL;
    hdr->proto = protocol;
    hdr->src = route.source;
    hdr->dst = dest;

    // Computed last, over the finished header with the field itself zeroed
    hdr->checksum = 0;
    hdr->checksum = htons(checksum(hdr, HEADER_LEN));

    pkt->mark_network_header();
    pkt->set_iface(route.iface);

    // The loopback interface is its own next hop
    if (route.type == route::route_type::local) {
        return eth::output(pkt, route.iface->mac(), eth::TYPE_IPV4);
    }

    if (route.type == route::route_type::broadcast) {
        return eth::output(pkt, eth::BROADCAST_ADDR, eth::TYPE_IPV4);
    }

    return arp::resolve_and_send(pkt, route.next_hop);
}

} // namespace ipv4
} // namespace net
