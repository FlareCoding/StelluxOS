#include "net/icmp.h"
#include "net/icmp_socket.h"
#include "net/interface.h"
#include "net/route.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "common/string.h"
#include "common/logging.h"

namespace net {
namespace icmp {

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
        log::warn("icmp: input called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());

    if (!iface || !ip) {
        log::warn("icmp: input called with a packet missing its interface or IP header");
        packet::free(pkt);
        return ERR_INVALID;
    }

    if (pkt->length() < HEADER_LEN) {
        return reject(iface, pkt, ERR_INVALID);
    }

    if (checksum(pkt->data(), pkt->length()) != 0) {
        return reject(iface, pkt, ERR_INVALID);
    }

    icmp_header* hdr = reinterpret_cast<icmp_header*>(pkt->data());
    const uint8_t* src = ip->src.bytes;

    switch (hdr->type) {
    case TYPE_ECHO_REQUEST: {
        // Requests sent to a broadcast address are ignored, answering them
        // would let one packet make every host on the link reply at once.
        if (ip->dst.is_broadcast() || iface->ipv4_conf().is_subnet_broadcast(ip->dst)) {
            packet::free(pkt);
            return OK;
        }

        // The request becomes the reply in place, only the type changes
        ipv4::ipv4_addr requester = ip->src;
        hdr->type = TYPE_ECHO_REPLY;

        return output(pkt, requester);
    }
    case TYPE_ECHO_REPLY: {
        socket_deliver(pkt);
        return OK;
    }
    case TYPE_DEST_UNREACHABLE:
    case TYPE_TIME_EXCEEDED:
    case TYPE_PARAMETER_PROBLEM:
        log::debug("icmp: error type %u code %u from %u.%u.%u.%u",
                  hdr->type, hdr->code, src[0], src[1], src[2], src[3]);
        packet::free(pkt);
        return OK;
    default:
        return drop(iface, pkt, OK);
    }
}

int32_t output(packet* pkt, const ipv4::ipv4_addr& dest) {
    if (!pkt) {
        log::warn("icmp: output called with no packet");
        return ERR_INVALID;
    }

    if (pkt->length() < HEADER_LEN) {
        log::warn("icmp: output called with a message shorter than its header");
        packet::free(pkt);
        return ERR_INVALID;
    }

    route::route_result route;
    int32_t rc = route::lookup(dest, &route);
    if (rc != OK) {
        packet::free(pkt);
        return rc;
    }

    icmp_header* hdr = reinterpret_cast<icmp_header*>(pkt->data());
    hdr->checksum = 0;
    hdr->checksum = htons(checksum(pkt->data(), pkt->length()));

    pkt->mark_transport_header();
    return ipv4::output(pkt, dest, route, ipv4::PROTO_ICMP);
}

int32_t send_echo_request(const ipv4::ipv4_addr& dest, uint16_t id, uint16_t seq,
                          const void* payload, size_t len) {
    if (len > 0 && !payload) {
        return ERR_INVALID;
    }

    packet* pkt = packet::alloc();
    if (!pkt) {
        return ERR_NO_MEMORY;
    }

    // Room for the two headers below, then the message itself
    uint8_t* body = nullptr;
    if (pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN)) {
        body = pkt->put(HEADER_LEN + len);
    }

    if (!body) {
        packet::free(pkt);
        return ERR_TOO_LARGE;
    }

    icmp_header* hdr = reinterpret_cast<icmp_header*>(body);
    hdr->type = TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->echo.id = htons(id);
    hdr->echo.seq = htons(seq);

    if (len > 0) {
        string::memcpy(body + HEADER_LEN, payload, len);
    }

    return output(pkt, dest);
}

// RFC 1122 3.2.2: an error message is never sent about another error message
static bool is_error_message(uint8_t type) {
    return type == TYPE_DEST_UNREACHABLE || type == TYPE_TIME_EXCEEDED ||
           type == TYPE_PARAMETER_PROBLEM;
}

int32_t send_error(const packet* offending, uint8_t type, uint8_t code) {
    if (!offending || offending->length() < ipv4::HEADER_LEN) {
        return ERR_INVALID;
    }

    interface* iface = offending->iface();
    const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(offending->data());
    if (!iface || offending->length() < ip->header_len()) {
        return ERR_INVALID;
    }

    // RFC 1122 3.2.2: silence unless the datagram went from one host to one host as a
    // link unicast and is a first fragment, or one packet could provoke a storm of errors
    ipv4::ipv4_config conf = iface->ipv4_conf();
    const eth::eth_header* link = reinterpret_cast<const eth::eth_header*>(offending->link_header());

    if (!conf.is_unicast(ip->src) || !conf.is_unicast(ip->dst) || ip->frag_off() != 0 ||
        (link && link->dest.is_multicast())) {
        return OK;
    }

    if (ip->proto == ipv4::PROTO_ICMP && offending->length() >= ip->header_len() + HEADER_LEN) {
        const icmp_header* inner =
            reinterpret_cast<const icmp_header*>(offending->data() + ip->header_len());
        if (is_error_message(inner->type)) {
            return OK;
        }
    }

    // The offending IP header and enough of its payload to hold the transport ports
    size_t included_len = ip->header_len() + ERROR_PAYLOAD_LEN;
    if (included_len > offending->length()) {
        included_len = offending->length();
    }

    packet* pkt = packet::alloc();
    if (!pkt) {
        return ERR_NO_MEMORY;
    }

    uint8_t* body = nullptr;
    if (pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN)) {
        body = pkt->put(HEADER_LEN + included_len);
    }

    if (!body) {
        packet::free(pkt);
        return ERR_TOO_LARGE;
    }

    icmp_header* hdr = reinterpret_cast<icmp_header*>(body);
    hdr->type = type;
    hdr->code = code;
    hdr->unused = 0;
    string::memcpy(body + HEADER_LEN, offending->data(), included_len);

    return output(pkt, ip->src);
}

} // namespace icmp
} // namespace net
