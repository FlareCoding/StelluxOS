#include "net/udp.h"
#include "net/udp_socket.h"
#include "net/icmp.h"
#include "net/net.h"
#include "net/interface.h"
#include "net/route.h"
#include "net/checksum.h"
#include "common/logging.h"

namespace net {
namespace udp {

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

/*
 * Checksum over the pseudo-header and `len` bytes of datagram. Over a received
 * datagram with its checksum in place the result is zero when it is intact.
 */
static uint16_t datagram_checksum(const ipv4::ipv4_addr& src, const ipv4::ipv4_addr& dst,
                                  const void* datagram, size_t len) {
    ipv4::pseudo_header pseudo = {};
    pseudo.src = src;
    pseudo.dst = dst;
    pseudo.zero = 0;
    pseudo.proto = ipv4::PROTO_UDP;
    pseudo.length = htons(static_cast<uint16_t>(len));

    uint32_t sum = checksum_accumulate(0, &pseudo, sizeof(pseudo));
    sum = checksum_accumulate(sum, datagram, len);

    return checksum_finish(sum);
}

int32_t input(packet* pkt) {
    if (!pkt) {
        log::warn("udp: input called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());

    if (!iface || !ip) {
        log::warn("udp: input called with a packet missing its interface or IP header");
        packet::free(pkt);
        return ERR_INVALID;
    }

    pkt->mark_transport_header();

    if (pkt->length() < HEADER_LEN) {
        return reject(iface, pkt, ERR_INVALID);
    }

    // The window may carry link padding past the datagram, never less than it
    const udp_header* hdr = reinterpret_cast<const udp_header*>(pkt->data());
    size_t length = ntohs(hdr->length);

    if (length < HEADER_LEN || length > pkt->length()) {
        return reject(iface, pkt, ERR_INVALID);
    }

    pkt->trim(length);

    if (hdr->checksum != CHECKSUM_NONE && datagram_checksum(ip->src, ip->dst, hdr, length) != 0) {
        return reject(iface, pkt, ERR_INVALID);
    }

    int32_t rc = socket_deliver(pkt);
    if (rc != ERR_NOT_FOUND) {
        return rc;
    }

    (void)pkt->push(ip->header_len());
    icmp::send_error(pkt, icmp::TYPE_DEST_UNREACHABLE, icmp::CODE_PORT_UNREACHABLE);

    return drop(iface, pkt, OK);
}

int32_t output(packet* pkt, interface* iface, const ipv4::ipv4_addr& dest,
               uint16_t src_port, uint16_t dest_port) {
    if (!pkt) {
        log::warn("udp: output called with no packet");
        return ERR_INVALID;
    }

    // The checksum covers the source address (which the route decides)
    route::route_result route;
    int32_t rc = iface ? route::lookup_on(iface, dest, &route) : route::lookup(dest, &route);
    if (rc != OK) {
        packet::free(pkt);
        return rc;
    }

    udp_header* hdr = reinterpret_cast<udp_header*>(pkt->push(HEADER_LEN));
    if (!hdr) {
        log::warn("udp: output packet has no headroom for the header");
        return reject(route.iface, pkt, ERR_INVALID);
    }

    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dest_port);
    hdr->length = htons(static_cast<uint16_t>(pkt->length()));
    hdr->checksum = 0;

    uint16_t sum = datagram_checksum(route.source, dest, hdr, pkt->length());
    hdr->checksum = htons(sum == 0 ? CHECKSUM_ALL_ONES : sum);

    pkt->mark_transport_header();
    return ipv4::output(pkt, dest, route, ipv4::PROTO_UDP);
}

} // namespace udp
} // namespace net
