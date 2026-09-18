#include "net/tcp/input.h"
#include "net/tcp/output.h"
#include "net/tcp/timers.h"
#include "net/tcp/seq.h"
#include "net/net.h"
#include "net/byteorder.h"
#include "resource/resource.h"
#include "random/random.h"
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

enum class segment_action : uint8_t {
    consume   = 0,
    challenge = 1,
    reset     = 2,
};

// The ACKs answered to unacceptable segments this second: the limit is drawn
// anew each second between half the maximum and the maximum, so a peer cannot
// learn it by counting
struct challenge_budget {
    uint64_t       window_start_ns;
    uint32_t       sent;
    uint32_t       limit;
    sync::spinlock lock;
};

static challenge_budget g_challenges = {0, 0, 0, sync::SPINLOCK_INIT};

bool take_challenge_ack() {
    bool allowed = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_challenges.lock);
        uint64_t now = now_ns();
        if (g_challenges.limit == 0 || now - g_challenges.window_start_ns >= CHALLENGE_ACK_WINDOW_NS) {
            uint32_t spread = 0;
            (void)random::fill(&spread, sizeof(spread));
            g_challenges.window_start_ns = now;
            g_challenges.sent = 0;
            g_challenges.limit = CHALLENGE_ACK_LIMIT / 2 + spread % (CHALLENGE_ACK_LIMIT / 2 + 1);
        }

        if (g_challenges.sent < g_challenges.limit) {
            g_challenges.sent++;
            allowed = true;
        }
    });

    return allowed;
}

// RFC 9293 3.4: whether the segment's bytes, or its bare position when it has
// none, fall inside the receive window. Caller holds the lock.
static bool is_acceptable_locked(const tcp_conn* conn, uint32_t seq, uint32_t seg_len) {
    uint32_t window_end = conn->rcv_nxt + conn->rcv_wnd;

    if (seg_len == 0) {
        return conn->rcv_wnd == 0 ? seq == conn->rcv_nxt
                                  : seq_geq(seq, conn->rcv_nxt) && seq_lt(seq, window_end);
    }

    if (conn->rcv_wnd == 0) {
        return false;
    }

    uint32_t last = seq + seg_len - 1;
    return (seq_geq(seq, conn->rcv_nxt) && seq_lt(seq, window_end)) ||
           (seq_geq(last, conn->rcv_nxt) && seq_lt(last, window_end));
}

// RFC 7323 5.3: a timestamp older than the newest seen rejects the segment,
// unless nothing was seen for so long that the clock may have wrapped. A
// reset is exempt, so an old clock cannot make one be ignored.
static bool paws_rejects_locked(const tcp_conn* conn, const tcp_options& opts) {
    return conn->ts_ok && opts.has_timestamps && seq_lt(opts.ts_val, conn->ts_recent) &&
           now_ns() - conn->ts_recent_age_ns < TS_RECENT_MAX_AGE_NS;
}

// RFC 9293 3.10.7.4 for an acceptable ACK: one below SND.UNA is a duplicate
// that changes nothing, otherwise the send window follows the newest segment
// (RFC 7323 4.3 for the timestamp). Caller holds the lock.
static void take_ack_locked(tcp_conn* conn, const tcp_header* hdr, const tcp_options& opts) {
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);
    bool current = seq_geq(ack, conn->snd_una);

    if (seq_gt(ack, conn->snd_una)) {
        conn->snd_una = ack;
    }

    if (current && (seq_lt(conn->snd_wl1, seq) || (conn->snd_wl1 == seq && seq_leq(conn->snd_wl2, ack)))) {
        conn->snd_wnd = uint32_t{ntohs(hdr->window)} << conn->snd_wscale;
        conn->snd_wl1 = seq;
        conn->snd_wl2 = ack;
        if (conn->snd_wnd > conn->max_snd_wnd) {
            conn->max_snd_wnd = conn->snd_wnd;
        }
    }

    if (conn->ts_ok && opts.has_timestamps && seq_leq(seq, conn->rcv_nxt)) {
        conn->ts_recent = opts.ts_val;
        conn->ts_recent_age_ns = now_ns();
    }
}

// RFC 9293 3.10.7.4 with RFC 5961: acceptability first, then a reset only at
// the expected sequence, a SYN never obeyed, and an ACK only within the range
// this host could have sent
static int32_t established_input(tcp_conn* conn, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);
    uint32_t seg_len = static_cast<uint32_t>(pkt->length() - hdr->header_len()) +
                       ((hdr->flags & FLAG_SYN) ? 1 : 0) + ((hdr->flags & FLAG_FIN) ? 1 : 0);
    segment_action action = segment_action::consume;
    segment_source src = {};

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        bool is_rst = hdr->flags & FLAG_RST;
        bool in_window = is_acceptable_locked(conn, seq, seg_len) && (is_rst || !paws_rejects_locked(conn, opts));

        if (conn->state != tcp_state::established) {
            action = segment_action::consume;
        } else if (!in_window) {
            action = (hdr->flags & FLAG_RST) ? segment_action::consume : segment_action::challenge;
        } else if (hdr->flags & FLAG_RST) {
            action = seq == conn->rcv_nxt ? segment_action::reset : segment_action::challenge;
        } else if (hdr->flags & FLAG_SYN) {
            action = segment_action::challenge;
        } else if (!(hdr->flags & FLAG_ACK)) {
            action = segment_action::consume;
        } else if (!seq_geq(ack, conn->snd_una - conn->max_snd_wnd) || !seq_leq(ack, conn->snd_nxt)) {
            action = segment_action::challenge;
        } else {
            take_ack_locked(conn, hdr, opts);
        }

        if (action == segment_action::reset) {
            conn->state = tcp_state::closed;
            conn->pending_error = resource::ERR_CONNRESET;
            conn->send_timer_kind = timer_kind::none;
        }

        src = snapshot_source(conn);
    });

    packet::free(pkt);

    if (action == segment_action::reset) {
        retire_connection(conn);
    } else if (action == segment_action::challenge && take_challenge_ack()) {
        return send_control(src, FLAG_ACK);
    }

    return OK;
}

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

        if (conn->state != tcp_state::syn_sent) {
            action = handshake_action::drop;
        } else if (has_ack && !ack_ok) {
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
        if (conn->state != tcp_state::syn_rcvd) {
            action = handshake_action::drop;
        } else if (hdr->flags & FLAG_RST) {
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
    case tcp_state::established:
        return established_input(conn, pkt, hdr, opts);
    default:
        packet::free(pkt);
        return OK;
    }
}

} // namespace tcp
} // namespace net
