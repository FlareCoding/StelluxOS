#include "net/tcp/tcp.h"
#include "net/tcp/wire.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/output.h"
#include "net/net.h"
#include "net/interface.h"
#include "net/eth.h"
#include "net/byteorder.h"
#include "common/logging.h"

namespace net {
namespace tcp {

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

// SEG.LEN counts the payload plus one for each of SYN and FIN (RFC 9293 3.4)
static uint32_t segment_len(const tcp_header* hdr, size_t total_len) {
    uint32_t len = static_cast<uint32_t>(total_len - hdr->header_len());
    if (hdr->flags & FLAG_SYN) {
        len++;
    }

    if (hdr->flags & FLAG_FIN) {
        len++;
    }

    return len;
}

// RFC 9293 3.10.7.1: the reset takes its sequence from the offending segment's
// acknowledgment, or acknowledges the segment when it carried none
static int32_t send_reset(interface* iface, const tuple& key, const tcp_header* offending,
                          size_t offending_len) {
    if (offending->flags & FLAG_ACK) {
        return send_segment(iface, key, FLAG_RST, ntohl(offending->ack), 0, 0, {});
    }

    uint32_t ack = ntohl(offending->seq) + segment_len(offending, offending_len);
    return send_segment(iface, key, FLAG_RST | FLAG_ACK, 0, ack, 0, {});
}

int32_t init() {
    int32_t rc = init_tables();
    if (rc != OK) {
        return rc;
    }

    return init_sequence_numbers();
}

int32_t input(packet* pkt) {
    if (!pkt) {
        log::warn("tcp: input called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());

    if (!iface || !ip) {
        log::warn("tcp: input called with a packet missing its interface or IP header");
        packet::free(pkt);
        return ERR_INVALID;
    }

    pkt->mark_transport_header();

    const tcp_header* hdr = reinterpret_cast<const tcp_header*>(pkt->data());
    if (!is_header_valid(hdr, pkt->length())) {
        return reject(iface, pkt, ERR_INVALID);
    }

    if (!is_checksum_valid(ip->src, ip->dst, hdr, pkt->length())) {
        return reject(iface, pkt, ERR_INVALID);
    }

    tcp_options opts;
    if (!parse_options(hdr, &opts)) {
        return reject(iface, pkt, ERR_INVALID);
    }

    // RFC 1122 4.2.3.10: a segment addressed to a broadcast or multicast address is discarded
    const eth::eth_header* link = reinterpret_cast<const eth::eth_header*>(pkt->link_header());
    if (!iface->ipv4_conf().is_unicast(ip->dst) || (link && link->dest.is_multicast())) {
        return drop(iface, pkt, OK);
    }

    tuple key = {ip->dst, ip->src, ntohs(hdr->dst_port), ntohs(hdr->src_port)};
    rc::strong_ref<record> rec = lookup(key);

    if (rec && rec->kind == record_kind::request) {
        return request_input(static_cast<tcp_request*>(rec.ptr()), pkt, hdr, opts);
    }

    if (rec) {
        packet::free(pkt);
        return OK;
    }

    bool syn_only = (hdr->flags & (FLAG_SYN | FLAG_ACK | FLAG_RST)) == FLAG_SYN;
    if (!rec && syn_only) {
        rc::strong_ref<tcp_listener> listener = listener_lookup(ip->dst, key.local_port, iface);
        if (listener) {
            return listen_input(listener.ptr(), pkt, hdr, opts);
        }
    }

    // Without a record to claim it, a segment is handled as in the CLOSED state
    if (hdr->flags & FLAG_RST) {
        packet::free(pkt);
        return OK;
    }

    int32_t rc = send_reset(iface, key, hdr, pkt->length());
    packet::free(pkt);

    return rc;
}

} // namespace tcp
} // namespace net
