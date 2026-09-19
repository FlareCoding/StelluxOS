#include "net/tcp/output.h"
#include "net/tcp/info.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/interface.h"
#include "net/route.h"
#include "net/byteorder.h"
#include "common/string.h"

namespace net {
namespace tcp {

uint16_t local_mss(const interface* iface) {
    return static_cast<uint16_t>(iface->mtu() - ipv4::HEADER_LEN - HEADER_LEN);
}

uint16_t window_field(uint32_t window, uint8_t wscale) {
    uint32_t scaled = window >> wscale;
    return static_cast<uint16_t>(scaled > 0xFFFF ? 0xFFFF : scaled);
}

segment_source snapshot_source(const tcp_conn* conn) {
    segment_source src = {};
    src.iface = conn->iface;
    src.key = conn->key;
    src.iss = conn->iss;
    src.snd_nxt = conn->snd_nxt;
    src.rcv_nxt = conn->rcv_nxt;
    src.rcv_wnd = conn->rcv_wnd;
    src.rcv_wscale = conn->rcv_wscale;
    src.wscale_ok = conn->wscale_ok;
    src.sack_ok = conn->sack_ok;
    src.ts_ok = conn->ts_ok;
    src.ts_offset = conn->ts_offset;
    src.ts_recent = conn->ts_recent;

    return src;
}

int32_t send_segment(interface* iface, const tuple& key, uint8_t flags, uint32_t seq, uint32_t ack,
                     uint16_t window, const tcp_options& opts) {
    route::route_result route;
    int32_t rc = route::lookup_on(iface, key.remote_addr, &route);
    if (rc != OK) {
        return rc;
    }

    route.source = key.local_addr;

    packet* pkt = packet::alloc();
    if (!pkt) {
        return ERR_NO_MEMORY;
    }

    uint8_t options[MAX_OPTIONS_LEN];
    size_t options_len = build_options(options, opts);
    size_t header_len = HEADER_LEN + options_len;

    tcp_header* hdr = nullptr;
    if (pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN)) {
        hdr = reinterpret_cast<tcp_header*>(pkt->put(header_len));
    }

    if (!hdr) {
        packet::free(pkt);
        return ERR_TOO_LARGE;
    }

    *hdr = {};
    hdr->src_port = htons(key.local_port);
    hdr->dst_port = htons(key.remote_port);
    hdr->seq = htonl(seq);
    hdr->ack = htonl(ack);
    hdr->set_data_offset(static_cast<uint8_t>(header_len / sizeof(uint32_t)));
    hdr->flags = flags;
    hdr->window = htons(window);
    string::memcpy(reinterpret_cast<uint8_t*>(hdr) + HEADER_LEN, options, options_len);
    hdr->checksum = htons(compute_checksum(key.local_addr, key.remote_addr, hdr, header_len));

    increment(counter::segments_out);
    if (flags & FLAG_RST) {
        increment(counter::rsts_sent);
    }

    return ipv4::output(pkt, key.remote_addr, route, ipv4::PROTO_TCP);
}

int32_t send_syn(const segment_source& src) {
    tcp_options opts;
    opts.mss = local_mss(src.iface);
    opts.sack_permitted = true;
    opts.has_timestamps = true;
    opts.ts_val = timestamp_value(src.ts_offset);
    opts.window_scale = src.rcv_wscale;

    return send_segment(src.iface, src.key, FLAG_SYN, src.iss, 0, window_field(src.rcv_wnd, 0), opts);
}

int32_t send_syn_ack(const segment_source& src) {
    tcp_options opts;
    opts.mss = local_mss(src.iface);
    opts.sack_permitted = src.sack_ok;
    opts.has_timestamps = src.ts_ok;
    opts.ts_val = timestamp_value(src.ts_offset);
    opts.ts_ecr = src.ts_recent;
    opts.window_scale = src.wscale_ok ? src.rcv_wscale : WINDOW_SCALE_NONE;

    return send_segment(src.iface, src.key, FLAG_SYN | FLAG_ACK, src.iss, src.rcv_nxt,
                        window_field(src.rcv_wnd, 0), opts);
}

static tcp_options control_options(const segment_source& src) {
    tcp_options opts;
    if (src.ts_ok) {
        opts.has_timestamps = true;
        opts.ts_val = timestamp_value(src.ts_offset);
        opts.ts_ecr = src.ts_recent;
    }

    return opts;
}

int32_t send_control(const segment_source& src, uint8_t flags) {
    return send_segment(src.iface, src.key, flags, src.snd_nxt, src.rcv_nxt,
                        window_field(src.rcv_wnd, src.rcv_wscale), control_options(src));
}

int32_t send_fin(const segment_source& src) {
    return send_segment(src.iface, src.key, FLAG_FIN | FLAG_ACK, src.snd_nxt - 1, src.rcv_nxt,
                        window_field(src.rcv_wnd, src.rcv_wscale), control_options(src));
}

} // namespace tcp
} // namespace net
