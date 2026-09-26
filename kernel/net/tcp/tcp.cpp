#include "net/tcp/tcp.h"
#include "net/tcp/wire.h"
#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/input.h"
#include "net/tcp/output.h"
#include "net/tcp/timewait.h"
#include "net/tcp/info.h"
#include "net/tcp/seq.h"
#include "net/icmp.h"
#include "net/net.h"
#include "net/interface.h"
#include "net/eth.h"
#include "net/byteorder.h"
#include "resource/resource.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"
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
    init_congestion();

    int32_t rc = init_tables();
    if (rc != OK) {
        return rc;
    }

    return init_sequence_numbers();
}

// RFC 1122 4.2.3.9 as RFC 5927 4.2 narrows it, OK for an error TCP ignores
static int32_t icmp_error_code(uint8_t type, uint8_t code) {
    switch (type) {
    case icmp::TYPE_DEST_UNREACHABLE:
        switch (code) {
        case icmp::CODE_NET_UNREACHABLE:
            return resource::ERR_NETUNREACH;
        case icmp::CODE_PROTOCOL_UNREACHABLE:
        case icmp::CODE_PORT_UNREACHABLE:
            return resource::ERR_CONNREFUSED;
        case icmp::CODE_FRAGMENTATION_NEEDED:
            return resource::OK;
        default:
            return resource::ERR_HOSTUNREACH;
        }
    case icmp::TYPE_TIME_EXCEEDED:
        return resource::ERR_HOSTUNREACH;
    case icmp::TYPE_PARAMETER_PROBLEM:
        return resource::ERR_PROTO;
    default:
        return resource::OK;
    }
}

void icmp_error(uint8_t type, uint8_t code, const ipv4::ipv4_header* inner, const uint8_t* segment,
                size_t segment_len) {
    int32_t error = icmp_error_code(type, code);
    if (error == resource::OK || segment_len < icmp::ERROR_PAYLOAD_LEN) {
        return;
    }

    const tcp_header* hdr = reinterpret_cast<const tcp_header*>(segment);
    tuple key = {inner->src, inner->dst, ntohs(hdr->src_port), ntohs(hdr->dst_port)};
    rc::strong_ref<record> rec = lookup(key);
    if (!rec || rec->kind != record_kind::connection) {
        return;
    }

    tcp_conn* conn = static_cast<tcp_conn*>(rec.ptr());
    uint32_t seq = ntohl(hdr->seq);
    bool handshake = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        bool outstanding = seq_geq(seq, conn->snd_una) && seq_lt(seq, conn->snd_nxt);
        if (outstanding) {
            handshake = conn->state == tcp_state::syn_sent || conn->state == tcp_state::syn_rcvd;
            if (is_synchronized(conn->state)) {
                conn->soft_error = error;
            }
        }
    });

    if (handshake) {
        fail_connection(conn, error);
    }
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
    increment(counter::segments_in);

    const tcp_header* hdr = reinterpret_cast<const tcp_header*>(pkt->data());
    if (!is_header_valid(hdr, pkt->length())) {
        return reject(iface, pkt, ERR_INVALID);
    }

    if (!is_checksum_valid(ip->src, ip->dst, hdr, pkt->length())) {
        increment(counter::checksum_failures);
        return reject(iface, pkt, ERR_INVALID);
    }

    tcp_options opts;
    if (!parse_options(hdr, &opts)) {
        return reject(iface, pkt, ERR_INVALID);
    }

    if (hdr->flags & FLAG_RST) {
        increment(counter::rsts_received);
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

    if (rec && rec->kind == record_kind::connection) {
        return conn_input(static_cast<tcp_conn*>(rec.ptr()), pkt, hdr, opts);
    }

    if (rec && rec->kind == record_kind::timewait) {
        int32_t rc = timewait_input(static_cast<tcp_timewait*>(rec.ptr()), pkt, hdr, opts);
        if (rc != TIMEWAIT_REOPEN) {
            return rc;
        }
    }

    bool syn_only = (hdr->flags & (FLAG_SYN | FLAG_ACK | FLAG_RST)) == FLAG_SYN;
    if (syn_only) {
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
