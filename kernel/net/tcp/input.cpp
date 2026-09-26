#include "net/tcp/input.h"
#include "net/tcp/output.h"
#include "net/tcp/timers.h"
#include "net/tcp/timewait.h"
#include "net/tcp/reassembly.h"
#include "net/tcp/recovery.h"
#include "net/tcp/rtt.h"
#include "net/tcp/info.h"
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
    abort     = 3,
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

    if (allowed) {
        increment(counter::challenge_acks);
    }

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
    bool rejects = conn->ts_ok && opts.has_timestamps && seq_lt(opts.ts_val, conn->ts_recent) &&
                   now_ns() - conn->ts_recent_age_ns < TS_RECENT_MAX_AGE_NS;

    if (rejects) {
        increment(counter::paws_drops);
    }

    return rejects;
}

// RFC 9293 3.10.7.4 for an acceptable ACK: one below SND.UNA is a duplicate
// that changes nothing, otherwise the send window follows the newest segment
// (RFC 7323 4.3 for the timestamp). Caller holds the lock.
static bool take_ack_locked(tcp_conn* conn, const tcp_header* hdr, const tcp_options& opts, size_t payload_len,
                            packet** retransmission) {
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);
    uint32_t window = uint32_t{ntohs(hdr->window)} << conn->snd_wscale;
    bool current = seq_geq(ack, conn->snd_una);
    bool duplicate = is_duplicate_ack_locked(conn, hdr->flags, payload_len, ack, window);
    bool freed_room = false;
    recovery_action action = recovery_action::none;
    
    conn->unanswered_probes = 0;
    conn->peer_acked_ns = now_ns();
    conn->soft_error = resource::OK;

    if (duplicate) {
        action = take_duplicate_ack_locked(conn);
    } else if (seq_gt(ack, conn->snd_una)) {
        uint32_t advance = ack - conn->snd_una;
        size_t queued = conn->snd_queue.size();

        acknowledged covered = conn->sent.acknowledge(ack);
        freed_room = conn->snd_queue.consume(advance < queued ? advance : queued) > 0;

        uint64_t rtt_ns = covered.rtt_sample_sent_ns != 0 ? now_ns() - covered.rtt_sample_sent_ns
                                                          : rtt_from_echo_locked(conn, opts.has_timestamps ? opts.ts_ecr : 0);
        if (rtt_ns != 0) {
            take_rtt_sample_locked(conn, rtt_ns);
        }

        conn->snd_una = ack;
        conn->retransmits = 0;
        conn->backoff = 0;
        action = take_advancing_ack_locked(conn, ack, covered.bytes);

        if (conn->congestion->acked) {
            conn->congestion->acked(conn, covered.bytes, rtt_ns != 0 ? static_cast<int64_t>(rtt_ns / 1000) : -1);
        }

        bool growing_state = conn->recovery == recovery_state::open || conn->recovery == recovery_state::loss;
        bool may_grow = action != recovery_action::recovered && growing_state;
        
        if (may_grow && is_cwnd_limited(conn)) {
            conn->congestion->grow(conn, covered.bytes);
        }

        if (conn->snd_una == conn->snd_nxt) {
            conn->send_timer_kind = timer_kind::none;
        } else {
            arm_send_timer_locked(conn, timer_kind::rto);
        }
    }

    if (action == recovery_action::retransmit) {
        *retransmission = rebuild_oldest_locked(conn);
    }

    if (current && (seq_lt(conn->snd_wl1, seq) || (conn->snd_wl1 == seq && seq_leq(conn->snd_wl2, ack)))) {
        conn->snd_wnd = window;
        conn->snd_wl1 = seq;
        conn->snd_wl2 = ack;
        if (conn->snd_wnd > conn->max_snd_wnd) {
            conn->max_snd_wnd = conn->snd_wnd;
        }
    }

    if (conn->send_timer_kind == timer_kind::probe && seq_lt(conn->snd_nxt, conn->snd_una + conn->snd_wnd)) {
        conn->send_timer_kind = timer_kind::none;
        conn->backoff = 0;
    }

    if (conn->ts_ok && opts.has_timestamps && seq_leq(seq, conn->rcv_nxt)) {
        conn->ts_recent = opts.ts_val;
        conn->ts_recent_age_ns = now_ns();
    }

    return freed_room;
}

// Caller holds the lock. RFC 9293 3.10.7.4: the acknowledgment
// of our FIN moves the close one state along.
static void take_fin_ack_locked(tcp_conn* conn, uint32_t ack) {
    if (!conn->fin_sent || !seq_geq(ack, conn->snd_nxt)) {
        return;
    }

    switch (conn->state) {
    case tcp_state::fin_wait_1:
        conn->send_timer_kind = timer_kind::none;
        conn->state = tcp_state::fin_wait_2;
        if (conn->orphaned) {
            arm_orphan_timer_locked(conn);
        }

        break;
    case tcp_state::closing:
        conn->send_timer_kind = timer_kind::none;
        conn->state = tcp_state::time_wait;
        break;
    case tcp_state::last_ack:
        conn->send_timer_kind = timer_kind::none;
        conn->state = tcp_state::closed;
        break;
    default:
        break;
    }
}

// Caller holds the lock. RFC 1122 4.2.2.13
static bool refuses_data_locked(const tcp_conn* conn, uint32_t seq, size_t payload_len) {
    bool receive_side_gone = conn->rcv_shutdown &&
                             (conn->state == tcp_state::fin_wait_1 || conn->state == tcp_state::fin_wait_2);

    return receive_side_gone && payload_len > 0 && seq_gt(seq + static_cast<uint32_t>(payload_len), conn->rcv_nxt);
}

// Caller holds the lock. RFC 5681 4.2: at once for a FIN, for payload out of
// order or filling a hole, after two full segments, in quick-ack mode, or
// after an idle period. Otherwise the ack timer.
static bool acknowledge_now_locked(tcp_conn* conn, size_t queued, size_t payload_len, bool fin) {
    uint64_t now = now_ns();
    bool idle = payload_len > 0 && conn->rcv_last_ns != 0 && now - conn->rcv_last_ns > conn->rto_ns;
    
    if (payload_len > 0) {
        conn->rcv_last_ns = now;
    }

    if (idle) {
        conn->quick_acks = MAX_QUICKACKS;
    }

    return fin || queued != payload_len || conn->quick_acks > 0 ||
           conn->rcv_nxt - conn->rcv_acked >= 2u * conn->rcv_mss;
}

// Caller holds the lock. Only a FIN right at rcv_nxt is counted, one
// behind payload not yet received waits for that payload.
static bool take_fin_locked(tcp_conn* conn, uint32_t fin_seq) {
    if (fin_seq != conn->rcv_nxt) {
        return false;
    }

    conn->rcv_nxt++;
    conn->fin_rcvd = true;
    clear_out_of_order_locked(conn);

    switch (conn->state) {
    case tcp_state::established:
        conn->state = tcp_state::close_wait;
        break;
    case tcp_state::fin_wait_1:
        conn->state = tcp_state::closing;
        break;
    case tcp_state::fin_wait_2:
        conn->state = tcp_state::time_wait;
        break;
    default:
        break;
    }

    return true;
}

static int32_t synchronized_input(tcp_conn* conn, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);
    size_t payload_len = pkt->length() - hdr->header_len();
    uint32_t seg_len = static_cast<uint32_t>(payload_len) +
                       ((hdr->flags & FLAG_SYN) ? 1 : 0) + ((hdr->flags & FLAG_FIN) ? 1 : 0);
    segment_action action = segment_action::consume;
    tcp_state before = tcp_state::closed;
    tcp_state after = tcp_state::closed;
    bool ack_now = false;
    bool wake_readers = false;
    bool wake_writers = false;
    bool try_send = false;
    bool packet_taken = false;
    packet* retransmission = nullptr;
    segment_source src = {};

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        bool is_rst = hdr->flags & FLAG_RST;
        bool in_window = is_acceptable_locked(conn, seq, seg_len) && (is_rst || !paws_rejects_locked(conn, opts));
        before = conn->state;

        if (!is_synchronized(conn->state)) {
            action = segment_action::consume;
        } else if (!in_window) {
            action = (hdr->flags & FLAG_RST) ? segment_action::consume : segment_action::challenge;
            
            if (payload_len > 0 && seq_lt(seq, conn->rcv_nxt)) {
                uint32_t old_end = seq + static_cast<uint32_t>(payload_len);
                note_dsack_locked(conn, seq, seq_lt(old_end, conn->rcv_nxt) ? old_end : conn->rcv_nxt);
            }
        } else if (hdr->flags & FLAG_RST) {
            action = seq == conn->rcv_nxt ? segment_action::reset : segment_action::challenge;
        } else if (hdr->flags & FLAG_SYN) {
            action = segment_action::challenge;
        } else if (!(hdr->flags & FLAG_ACK)) {
            action = segment_action::consume;
        } else if (!seq_geq(ack, conn->snd_una - conn->max_snd_wnd) || !seq_leq(ack, conn->snd_nxt)) {
            action = segment_action::challenge;
        } else {
            wake_writers = take_ack_locked(conn, hdr, opts, payload_len, &retransmission);
            take_fin_ack_locked(conn, ack);

            if (refuses_data_locked(conn, seq, payload_len)) {
                action = segment_action::abort;
            } else {
                try_send = unsent_bytes(conn) > 0 || conn->fin_pending || conn->sent.lost_bytes() > 0;

                payload_result got = take_payload_locked(conn, pkt, hdr, seq, payload_len);
                packet_taken = got.packet_taken;
                bool fin_taken = got.fin_seq != 0 && take_fin_locked(conn, got.fin_seq);
                wake_readers = got.queued > 0 || fin_taken;

                if (payload_len > 0 || (hdr->flags & FLAG_FIN)) {
                    ack_now = acknowledge_now_locked(conn, got.queued, payload_len, hdr->flags & FLAG_FIN);
                    if (!ack_now) {
                        delay_ack_locked(conn);
                    }
                }
            }
        }

        if (action == segment_action::reset || action == segment_action::abort) {
            conn->state = tcp_state::closed;
            conn->pending_error = resource::ERR_CONNRESET;
            conn->send_timer_kind = timer_kind::none;
        }

        after = conn->state;
        src = snapshot_source(conn);
        if (ack_now) {
            mark_ack_sent_locked(conn);
        }
    });

    if (!packet_taken) {
        packet::free(pkt);
    }

    int32_t rc = OK;
    if (action == segment_action::abort) {
        rc = send_segment(src.iface, src.key, FLAG_RST | FLAG_ACK, src.snd_nxt, src.rcv_nxt, 0, {});
    } else if (ack_now) {
        rc = send_control(src, FLAG_ACK);
    } else if (action == segment_action::challenge && take_challenge_ack()) {
        rc = send_control(src, FLAG_ACK);
        
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            conn->dsack_pending = false;
        });
    }

    if (wake_readers) {
        RUN_ELEVATED(sync::wake_all(conn->rx_wq));
    }

    if (wake_writers) {
        RUN_ELEVATED(sync::wake_all(conn->tx_wq));
    }

    if (retransmission) {
        increment(counter::retransmits);
        (void)transmit_segment(retransmission, src.iface, src.key);
    }

    if (try_send && is_synchronized(after)) {
        (void)output(conn);
    }

    if (after == tcp_state::time_wait) {
        enter_time_wait(conn);
    } else if (after == tcp_state::closed) {
        retire_connection(conn);
    } else if (after != before) {
        RUN_ELEVATED(sync::wake_all(conn->conn_wq));
    }

    return rc;
}

// Caller holds the lock. RFC 9293 3.10.7.3: what a SYN,
// alone or with its ACK, tells a connection that sent one.
static void take_peer_syn_locked(tcp_conn* conn, const tcp_header* hdr, const tcp_options& opts) {
    uint32_t seq = ntohl(hdr->seq);
    conn->irs = seq;
    conn->rcv_nxt = seq + 1;
    conn->rcv_mss = opts.mss != MSS_NONE ? opts.mss : DEFAULT_MSS;
    conn->snd_mss = send_mss(conn->iface, conn->rcv_mss, conn->snd_mss_cap);
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
    conn->rcv_adv = conn->rcv_nxt + conn->rcv_wnd;
    conn->rcv_acked = conn->rcv_nxt;
    configure_send_path_locked(conn);

    uint64_t rtt_ns = rtt_from_echo_locked(conn, opts.has_timestamps ? opts.ts_ecr : 0);
    if (rtt_ns != 0) {
        take_rtt_sample_locked(conn, rtt_ns);
    }
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
                conn->retransmits = 0;
                conn->backoff = 0;
                conn->soft_error = resource::OK;
                arm_keepalive_locked(conn);
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
        RUN_ELEVATED({
            sync::wake_all(conn->conn_wq);
            sync::wake_all(conn->tx_wq);
        });
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
            conn->retransmits = 0;
            conn->backoff = 0;
            conn->soft_error = resource::OK;
            action = handshake_action::established;
        }
    });

    packet::free(pkt);

    if (action == handshake_action::refused) {
        fail_connection(conn, resource::ERR_CONNREFUSED);
    } else if (action == handshake_action::established) {
        disarm_timer(conn, &conn->send_timer);
        RUN_ELEVATED({
            sync::wake_all(conn->conn_wq);
            sync::wake_all(conn->tx_wq);
        });
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
    case tcp_state::fin_wait_1:
    case tcp_state::fin_wait_2:
    case tcp_state::close_wait:
    case tcp_state::closing:
    case tcp_state::last_ack:
        return synchronized_input(conn, pkt, hdr, opts);
    default:
        packet::free(pkt);
        return OK;
    }
}

} // namespace tcp
} // namespace net
