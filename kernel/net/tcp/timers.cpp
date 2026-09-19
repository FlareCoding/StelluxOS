#include "net/tcp/timers.h"
#include "net/tcp/output.h"
#include "net/tcp/info.h"
#include "net/net.h"
#include "resource/resource.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

enum class send_action : uint8_t {
    none         = 0,
    retransmit   = 1,
    give_up      = 2,
    reset        = 3,
};

static void release(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

void arm_timer(record* rec, timer::deadline_timer* timer, uint64_t deadline_ns) {
    rec->add_ref();

    bool moved = false;
    RUN_ELEVATED(moved = timer::schedule(timer, deadline_ns));
    if (moved) {
        release(rec);
    }
}

void disarm_timer(record* rec, timer::deadline_timer* timer) {
    timer::cancel_outcome outcome = timer::cancel_outcome::not_scheduled;
    RUN_ELEVATED(outcome = timer::cancel(timer));

    if (outcome == timer::cancel_outcome::removed) {
        release(rec);
    }
}

void finish_timer_callback(record* rec) {
    release(rec);
}

void arm_send_timer_locked(tcp_conn* conn, timer_kind kind) {
    uint64_t backoff = TIMEOUT_INIT_NS << conn->retransmits;
    conn->send_timer_kind = kind;
    conn->send_timer_deadline_ns = now_ns() + (backoff > TIMEOUT_MAX_NS ? TIMEOUT_MAX_NS : backoff);
    arm_timer(conn, &conn->send_timer, conn->send_timer_deadline_ns);
}

void arm_orphan_timer_locked(tcp_conn* conn) {
    conn->send_timer_kind = timer_kind::orphan;
    conn->send_timer_deadline_ns = now_ns() + FIN_TIMEOUT_NS;
    arm_timer(conn, &conn->send_timer, conn->send_timer_deadline_ns);
}

static uint8_t retry_limit(tcp_state state) {
    return state == tcp_state::syn_sent || state == tcp_state::syn_rcvd ? SYN_RETRIES : ORPHAN_RETRIES;
}

static int32_t retransmit(tcp_state state, const segment_source& src) {
    switch (state) {
    case tcp_state::syn_sent:
        return send_syn(src);
    case tcp_state::syn_rcvd:
        return send_syn_ack(src);
    case tcp_state::fin_wait_1:
    case tcp_state::closing:
    case tcp_state::last_ack:
        return send_fin(src);
    default:
        return OK;
    }
}

void on_send_timer(timer::deadline_timer* timer) {
    tcp_conn* conn = timer::owner_of<tcp_conn, &tcp_conn::send_timer>(timer);
    send_action action = send_action::none;
    tcp_state state = tcp_state::closed;
    segment_source src = {};

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (conn->send_timer_kind != timer_kind::none) {
            if (now_ns() < conn->send_timer_deadline_ns) {
                arm_timer(conn, &conn->send_timer, conn->send_timer_deadline_ns);
                finish_timer_callback(conn);
                return;
            }

            state = conn->state;
            timer_kind kind = conn->send_timer_kind;
            conn->send_timer_kind = timer_kind::none;
            if (kind == timer_kind::orphan) {
                src = snapshot_source(conn);
                conn->state = tcp_state::closed;
                conn->pending_error = resource::ERR_TIMEDOUT;
                action = send_action::reset;
            } else if (conn->retransmits >= retry_limit(state)) {
                conn->state = tcp_state::closed;
                conn->pending_error = resource::ERR_TIMEDOUT;
                action = send_action::give_up;
            } else {
                conn->retransmits++;
                src = snapshot_source(conn);
                arm_send_timer_locked(conn, timer_kind::rto);
                action = send_action::retransmit;
            }
        }
    });

    if (action == send_action::reset) {
        (void)send_segment(src.iface, src.key, FLAG_RST | FLAG_ACK, src.snd_nxt, src.rcv_nxt, 0, {});
        retire_connection(conn);
    } else if (action == send_action::give_up) {
        retire_connection(conn);
    } else if (action == send_action::retransmit) {
        increment(counter::retransmits);
        (void)retransmit(state, src);
    }

    finish_timer_callback(conn);
}

} // namespace tcp
} // namespace net
