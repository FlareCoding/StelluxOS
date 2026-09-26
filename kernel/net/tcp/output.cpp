#include "net/tcp/output.h"
#include "net/tcp/info.h"
#include "net/tcp/reassembly.h"
#include "net/tcp/timers.h"
#include "net/tcp/seq.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/interface.h"
#include "net/route.h"
#include "net/byteorder.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"
#include "common/string.h"

namespace net {
namespace tcp {

uint16_t local_mss(const interface* iface) {
    return static_cast<uint16_t>(iface->mtu() - ipv4::HEADER_LEN - HEADER_LEN);
}

uint16_t send_mss(const interface* iface, uint16_t peer_mss, uint16_t cap) {
    uint16_t mss = peer_mss < local_mss(iface) ? peer_mss : local_mss(iface);

    return cap != 0 && cap < mss ? cap : mss;
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
    src.snd_una = conn->snd_una;
    src.snd_nxt = conn->snd_nxt;
    src.rcv_nxt = conn->rcv_nxt;
    src.rcv_wnd = conn->rcv_wnd;
    src.rcv_wscale = conn->rcv_wscale;
    src.wscale_ok = conn->wscale_ok;
    src.sack_ok = conn->sack_ok;
    src.ts_ok = conn->ts_ok;
    src.ts_offset = conn->ts_offset;
    src.ts_recent = conn->ts_recent;

    tcp_options sacks;
    fill_sack_blocks_locked(conn, &sacks);
    src.sack_count = sacks.sack_count;
    for (uint8_t i = 0; i < sacks.sack_count; i++) {
        src.sack_blocks[i] = sacks.sack_blocks[i];
    }

    return src;
}

static packet* build_segment(const tuple& key, uint8_t flags, uint32_t seq, uint32_t ack, uint16_t window,
                             const tcp_options& opts, size_t payload_len, uint8_t** payload) {
    packet* pkt = packet::alloc();
    if (!pkt) {
        return nullptr;
    }

    uint8_t options[MAX_OPTIONS_LEN];
    size_t options_len = build_options(options, opts);
    size_t header_len = HEADER_LEN + options_len;

    tcp_header* hdr = nullptr;
    if (pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN)) {
        hdr = reinterpret_cast<tcp_header*>(pkt->put(header_len + payload_len));
    }

    if (!hdr) {
        packet::free(pkt);
        return nullptr;
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
    *payload = reinterpret_cast<uint8_t*>(hdr) + header_len;

    return pkt;
}

int32_t transmit_segment(packet* pkt, interface* iface, const tuple& key) {
    route::route_result route;
    int32_t rc = route::lookup_on(iface, key.remote_addr, &route);
    if (rc != OK) {
        packet::free(pkt);
        return rc;
    }

    route.source = key.local_addr;

    tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
    hdr->checksum = 0;
    hdr->checksum = htons(compute_checksum(key.local_addr, key.remote_addr, hdr, pkt->length()));

    increment(counter::segments_out);
    if (hdr->flags & FLAG_RST) {
        increment(counter::rsts_sent);
    }

    return ipv4::output(pkt, key.remote_addr, route, ipv4::PROTO_TCP);
}

int32_t send_segment(interface* iface, const tuple& key, uint8_t flags, uint32_t seq, uint32_t ack,
                     uint16_t window, const tcp_options& opts) {
    uint8_t* payload = nullptr;
    packet* pkt = build_segment(key, flags, seq, ack, window, opts, 0, &payload);
    if (!pkt) {
        return ERR_NO_MEMORY;
    }

    return transmit_segment(pkt, iface, key);
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

    opts.sack_count = src.sack_count;
    for (uint8_t i = 0; i < src.sack_count; i++) {
        opts.sack_blocks[i] = src.sack_blocks[i];
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

int32_t send_probe(const segment_source& src) {
    return send_segment(src.iface, src.key, FLAG_ACK, src.snd_una - 1, src.rcv_nxt,
                        window_field(src.rcv_wnd, src.rcv_wscale), control_options(src));
}

packet* build_data_segment(const tcp_conn* conn, uint32_t seq, size_t len, uint8_t flags) {
    segment_source src = snapshot_source(conn);
    uint8_t* payload = nullptr;
    packet* pkt = build_segment(src.key, flags, seq, src.rcv_nxt, window_field(src.rcv_wnd, src.rcv_wscale),
                                control_options(src), len, &payload);
    if (!pkt) {
        return nullptr;
    }

    (void)conn->snd_queue.copy_out(seq - conn->snd_una, payload, len);

    return pkt;
}

// Caller holds the lock. RFC 6691: the MSS less the option bytes in use
static size_t payload_mss(const tcp_conn* conn) {
    uint8_t scratch[MAX_OPTIONS_LEN];
    size_t options = build_options(scratch, control_options(snapshot_source(conn)));
    return conn->snd_mss > options ? conn->snd_mss - options : 1;
}

static bool may_send(tcp_state state) {
    return state == tcp_state::established || state == tcp_state::close_wait ||
           state == tcp_state::fin_wait_1 || state == tcp_state::closing || state == tcp_state::last_ack;
}

// Caller holds the lock. Sender SWS avoidance (RFC 9293 3.8.6.2.1), then
// Nagle with Minshall's modification (RFC 1122 4.2.3.4).
static bool may_send_partial_locked(const tcp_conn* conn, uint32_t usable) {
    bool nothing_in_flight = conn->snd_nxt == conn->snd_una;
    if (nothing_in_flight) {
        return true;
    }

    uint32_t sws_floor = conn->max_snd_wnd / 2 < conn->snd_mss ? conn->max_snd_wnd / 2 : conn->snd_mss;
    if (usable < sws_floor) {
        return false;
    }

    bool small_outstanding = seq_gt(conn->snd_sml, conn->snd_una) && seq_leq(conn->snd_sml, conn->snd_nxt);
    return conn->nodelay || !small_outstanding;
}

static size_t build_burst_locked(tcp_conn* conn, packet** burst, size_t max) {
    size_t count = 0;
    uint64_t now = now_ns();
    size_t mss = payload_mss(conn);

    while (count < max) {
        size_t available = unsent_bytes(conn);
        if (available == 0) {
            break;
        }

        if (conn->snd_nxt == conn->snd_una) {
            begin_transmission_locked(conn, now);
        }

        uint32_t in_flight = conn->snd_nxt - conn->snd_una;
        uint32_t window_end = conn->snd_una + conn->snd_wnd;
        uint32_t usable = seq_lt(conn->snd_nxt, window_end) ? window_end - conn->snd_nxt : 0;
        uint32_t cwnd_room = conn->cwnd > in_flight ? conn->cwnd - in_flight : 0;
        uint32_t allowed = usable < cwnd_room ? usable : cwnd_room;
        size_t len = available < allowed ? available : allowed;
        if (len > mss) {
            len = mss;
        }

        bool last = len == available;
        bool carries_fin = last && conn->fin_pending && !conn->fin_sent && usable > len;
        if (len == 0 || (len < mss && !carries_fin && !may_send_partial_locked(conn, usable))) {
            break;
        }

        uint8_t flags = FLAG_ACK | (last ? FLAG_PSH : 0) | (carries_fin ? FLAG_FIN : 0);
        packet* pkt = build_data_segment(conn, conn->snd_nxt, len, flags);
        if (!pkt) {
            break;
        }

        if (!conn->sent.track(conn->snd_nxt, conn->snd_nxt + static_cast<uint32_t>(len), now)) {
            packet::free(pkt);
            break;
        }

        if (len < mss) {
            conn->snd_sml = conn->snd_nxt + static_cast<uint32_t>(len);
        }

        conn->snd_nxt += static_cast<uint32_t>(len);
        conn->last_data_sent_ns = now;
        if (carries_fin) {
            conn->fin_pending = false;
            conn->fin_sent = true;
            conn->snd_nxt++;
        }

        if (conn->send_timer_kind == timer_kind::none) {
            arm_send_timer_locked(conn, timer_kind::rto);
        }

        mark_ack_sent_locked(conn);
        burst[count++] = pkt;
    }

    bool fin_fits = seq_lt(conn->snd_nxt, conn->snd_una + conn->snd_wnd);
    if (count < max && conn->fin_pending && !conn->fin_sent && unsent_bytes(conn) == 0 && fin_fits) {
        conn->fin_pending = false;
        conn->fin_sent = true;
        conn->snd_nxt++;
        if (conn->send_timer_kind == timer_kind::none) {
            conn->retransmits = 0;
            conn->backoff = 0;
            arm_send_timer_locked(conn, timer_kind::rto);
        }

        mark_ack_sent_locked(conn);
        segment_source src = snapshot_source(conn);
        uint8_t* payload = nullptr;
        packet* pkt = build_segment(src.key, FLAG_FIN | FLAG_ACK, src.snd_nxt - 1, src.rcv_nxt,
                                    window_field(src.rcv_wnd, src.rcv_wscale), control_options(src), 0, &payload);
        if (pkt) {
            burst[count++] = pkt;
        }
    }

    bool blocked = unsent_bytes(conn) > 0 || (conn->fin_pending && !conn->fin_sent);
    if (blocked && conn->snd_nxt == conn->snd_una && conn->send_timer_kind == timer_kind::none) {
        arm_send_timer_locked(conn, timer_kind::probe);
    }

    uint32_t in_flight = conn->snd_nxt - conn->snd_una;
    bool cwnd_limited = unsent_bytes(conn) > 0 && conn->cwnd - in_flight < mss;

    if (count > 0 || cwnd_limited) {
        note_cwnd_usage_locked(conn, cwnd_limited);
    }

    return count;
}

int32_t output(tcp_conn* conn) {
    packet* burst[MAX_BURST];
    size_t count = 0;
    interface* iface = conn->iface;
    tuple key = conn->key;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (may_send(conn->state)) {
            count = build_burst_locked(conn, burst, MAX_BURST);
        }
    });

    for (size_t i = 0; i < count; i++) {
        (void)transmit_segment(burst[i], iface, key);
    }

    return OK;
}

} // namespace tcp
} // namespace net
