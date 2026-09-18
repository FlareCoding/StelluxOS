#include "net/tcp/input.h"
#include "net/tcp/output.h"
#include "net/tcp/timers.h"
#include "net/tcp/seq.h"
#include "net/net.h"
#include "net/byteorder.h"
#include "resource/resource.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

enum class handshake_action : uint8_t {
    drop         = 0,
    reset_peer   = 1,
    refused      = 2,
    established  = 3,
    simultaneous = 4,
};

// Caller holds the lock. RFC 9293 3.10.7.3: what a SYN,
// alone or with its ACK, tells a connection that sent one.
static void take_peer_syn_locked(tcp_conn* conn, const tcp_header* hdr, const tcp_options& opts) {
    uint32_t seq = ntohl(hdr->seq);
    conn->irs = seq;
    conn->rcv_nxt = seq + 1;
    conn->rcv_mss = opts.mss != MSS_NONE ? opts.mss : DEFAULT_MSS;
    conn->snd_mss = conn->rcv_mss < local_mss(conn->iface) ? conn->rcv_mss : local_mss(conn->iface);
    conn->wscale_ok = opts.window_scale != WINDOW_SCALE_NONE;
    conn->snd_wscale = conn->wscale_ok ? opts.window_scale : 0;
    conn->rcv_wscale = conn->wscale_ok ? conn->rcv_wscale : 0;
    conn->sack_ok = opts.sack_permitted;
    conn->ts_ok = opts.has_timestamps;
    conn->ts_recent = opts.ts_val;
    conn->ts_recent_age_ns = now_ns();
    conn->snd_wnd = ntohs(hdr->window);
    conn->snd_wl1 = seq;
    conn->snd_wl2 = ntohl(hdr->ack);
    conn->max_snd_wnd = conn->snd_wnd;
}

static int32_t syn_sent_input(tcp_conn* conn, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    uint32_t ack = ntohl(hdr->ack);
    bool has_ack = hdr->flags & FLAG_ACK;

    handshake_action action = handshake_action::drop;
    segment_source src = {};

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        bool ack_ok = has_ack && seq_gt(ack, conn->iss) && seq_leq(ack, conn->snd_nxt);

        if (has_ack && !ack_ok) {
            action = (hdr->flags & FLAG_RST) ? handshake_action::drop : handshake_action::reset_peer;
        } else if (hdr->flags & FLAG_RST) {
            action = ack_ok ? handshake_action::refused : handshake_action::drop;
        } else if (hdr->flags & FLAG_SYN) {
            take_peer_syn_locked(conn, hdr, opts);

            if (ack_ok) {
                conn->snd_una = ack;
                conn->state = tcp_state::established;
                conn->send_timer_kind = timer_kind::none;
                action = handshake_action::established;
            } else {
                conn->state = tcp_state::syn_rcvd;
                action = handshake_action::simultaneous;
            }

            src = snapshot_source(conn);
        }
    });

    packet::free(pkt);

    switch (action) {
    case handshake_action::reset_peer:
        return send_segment(conn->iface, conn->key, FLAG_RST, ack, 0, 0, {});
    case handshake_action::refused:
        fail_connection(conn, resource::ERR_CONNREFUSED);
        return OK;
    case handshake_action::established:
        disarm_timer(conn, &conn->send_timer);
        RUN_ELEVATED(sync::wake_all(conn->conn_wq));
        return send_control(src, FLAG_ACK);
    case handshake_action::simultaneous:
        return send_syn_ack(src);
    case handshake_action::drop:
        break;
    }

    return OK;
}

// RFC 9293 3.10.7.4 for a connection that opened actively and met a SYN:
// the peer's ACK of the SYN-ACK completes it, a reset at the expected
// sequence refuses it
static int32_t syn_rcvd_input(tcp_conn* conn, packet* pkt, const tcp_header* hdr, const tcp_options&) {
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);
    handshake_action action = handshake_action::drop;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (hdr->flags & FLAG_RST) {
            action = seq == conn->rcv_nxt ? handshake_action::refused : handshake_action::drop;
        } else if ((hdr->flags & FLAG_ACK) && seq_gt(ack, conn->snd_una) && seq_leq(ack, conn->snd_nxt)) {
            conn->snd_una = ack;
            conn->snd_wnd = uint32_t{ntohs(hdr->window)} << conn->snd_wscale;
            conn->snd_wl1 = seq;
            conn->snd_wl2 = ack;
            conn->max_snd_wnd = conn->snd_wnd;
            conn->state = tcp_state::established;
            conn->send_timer_kind = timer_kind::none;
            action = handshake_action::established;
        }
    });

    packet::free(pkt);

    if (action == handshake_action::refused) {
        fail_connection(conn, resource::ERR_CONNREFUSED);
    } else if (action == handshake_action::established) {
        disarm_timer(conn, &conn->send_timer);
        RUN_ELEVATED(sync::wake_all(conn->conn_wq));
    }

    return OK;
}

int32_t conn_input(tcp_conn* conn, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    tcp_state state;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        state = conn->state;
    });

    switch (state) {
    case tcp_state::syn_sent:
        return syn_sent_input(conn, pkt, hdr, opts);
    case tcp_state::syn_rcvd:
        return syn_rcvd_input(conn, pkt, hdr, opts);
    default:
        packet::free(pkt);
        return OK;
    }
}

} // namespace tcp
} // namespace net
