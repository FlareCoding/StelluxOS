#include "net/tcp/timers.h"
#include "net/tcp/output.h"
#include "resource/resource.h"
#include "sync/spinlock.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

enum class send_action : uint8_t {
    none         = 0,
    retransmit   = 1,
    give_up      = 2,
};

static void release(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

void arm_timer(record* rec, timer::deadline_timer* timer, uint64_t deadline_ns) {
    rec->add_ref();
    RUN_ELEVATED(timer::schedule(timer, deadline_ns));
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
            conn->send_timer_kind = timer_kind::none;
            if (conn->retransmits >= SYN_RETRIES) {
                action = send_action::give_up;
            } else {
                conn->retransmits++;
                src = snapshot_source(conn);
                arm_send_timer_locked(conn, timer_kind::rto);
                action = send_action::retransmit;
            }
        }
    });

    if (action == send_action::give_up) {
        fail_connection(conn, resource::ERR_TIMEDOUT);
    } else if (action == send_action::retransmit && state == tcp_state::syn_sent) {
        (void)send_syn(src);
    } else if (action == send_action::retransmit && state == tcp_state::syn_rcvd) {
        (void)send_syn_ack(src);
    }

    finish_timer_callback(conn);
}

} // namespace tcp
} // namespace net
