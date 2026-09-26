#include "net/tcp/timers.h"
#include "net/tcp/recovery.h"
#include "net/tcp/output.h"
#include "net/tcp/info.h"
#include "net/tcp/seq.h"
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
    probe        = 4,
    push         = 5,
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

// Caller holds the lock. RFC 6298 5.5, stopping once the wait reaches TIMEOUT_MAX_NS
static void back_off_locked(tcp_conn* conn) {
    if ((conn->rto_ns << conn->backoff) < TIMEOUT_MAX_NS) {
        conn->backoff++;
    }
}

void arm_send_timer_locked(tcp_conn* conn, timer_kind kind) {
    uint64_t wait = conn->rto_ns << conn->backoff;
    conn->send_timer_kind = kind;
    conn->send_timer_deadline_ns = now_ns() + (wait > TIMEOUT_MAX_NS ? TIMEOUT_MAX_NS : wait);

    arm_timer(conn, &conn->send_timer, conn->send_timer_deadline_ns);
}

void arm_orphan_timer_locked(tcp_conn* conn) {
    conn->send_timer_kind = timer_kind::orphan;
    conn->send_timer_deadline_ns = now_ns() + FIN_TIMEOUT_NS;

    arm_timer(conn, &conn->send_timer, conn->send_timer_deadline_ns);
}

void delay_ack_locked(tcp_conn* conn) {
    conn->ack_pending = true;
    if (conn->ack_timer_armed) {
        return;
    }

    conn->ack_timer_armed = true;
    conn->ack_timer_deadline_ns = now_ns() + DELACK_NS;
    
    arm_timer(conn, &conn->ack_timer, conn->ack_timer_deadline_ns);
}

void on_ack_timer(timer::deadline_timer* timer) {
    tcp_conn* conn = timer::owner_of<tcp_conn, &tcp_conn::ack_timer>(timer);
    bool send = false;
    segment_source src = {};

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (conn->ack_timer_armed) {
            if (now_ns() < conn->ack_timer_deadline_ns) {
                arm_timer(conn, &conn->ack_timer, conn->ack_timer_deadline_ns);
                finish_timer_callback(conn);
                return;
            }

            conn->ack_timer_armed = false;
            if (conn->ack_pending && is_synchronized(conn->state)) {
                src = snapshot_source(conn);
                mark_ack_sent_locked(conn);
                send = true;
            }
        }
    });

    if (send) {
        (void)send_control(src, FLAG_ACK);
    }

    finish_timer_callback(conn);
}

void arm_keepalive_locked(tcp_conn* conn) {
    if (!conn->keepalive || !is_synchronized(conn->state)) {
        return;
    }

    uint64_t now = now_ns();
    uint64_t idle_end = conn->peer_acked_ns + static_cast<uint64_t>(conn->keepalive_idle_s) * 1000000000ULL;
    conn->keepalive_armed = true;
    conn->keepalive_deadline_ns = idle_end > now ? idle_end : now;
    arm_timer(conn, &conn->keepalive_timer, conn->keepalive_deadline_ns);
}

// Caller holds the lock
static void arm_keepalive_after_locked(tcp_conn* conn, uint64_t wait_ns) {
    conn->keepalive_armed = true;
    conn->keepalive_deadline_ns = now_ns() + wait_ns;
    arm_timer(conn, &conn->keepalive_timer, conn->keepalive_deadline_ns);
}

void on_keepalive_timer(timer::deadline_timer* timer) {
    tcp_conn* conn = timer::owner_of<tcp_conn, &tcp_conn::keepalive_timer>(timer);
    send_action action = send_action::none;
    segment_source src = {};

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (conn->keepalive_armed) {
            if (now_ns() < conn->keepalive_deadline_ns) {
                arm_timer(conn, &conn->keepalive_timer, conn->keepalive_deadline_ns);
                finish_timer_callback(conn);
                return;
            }

            conn->keepalive_armed = false;
            uint64_t idle_ns = static_cast<uint64_t>(conn->keepalive_idle_s) * 1000000000ULL;
            uint64_t interval_ns = static_cast<uint64_t>(conn->keepalive_interval_s) * 1000000000ULL;
            if (conn->keepalive && is_synchronized(conn->state)) {
                if (conn->snd_nxt != conn->snd_una) {
                    arm_keepalive_after_locked(conn, idle_ns);
                } else if (now_ns() - conn->peer_acked_ns < idle_ns) {
                    arm_keepalive_locked(conn);
                } else if (conn->unanswered_probes >= conn->keepalive_probes) {
                    conn->state = tcp_state::closed;
                    conn->pending_error = conn->soft_error != resource::OK ? conn->soft_error : resource::ERR_TIMEDOUT;
                    src = snapshot_source(conn);
                    action = send_action::reset;
                } else {
                    conn->unanswered_probes++;
                    src = snapshot_source(conn);
                    mark_ack_sent_locked(conn);
                    arm_keepalive_after_locked(conn, interval_ns);
                    action = send_action::probe;
                }
            }
        }
    });

    if (action == send_action::reset) {
        (void)send_segment(src.iface, src.key, FLAG_RST | FLAG_ACK, src.snd_nxt, src.rcv_nxt, 0, {});
        retire_connection(conn);
    } else if (action == send_action::probe) {
        (void)send_probe(src);
    }

    finish_timer_callback(conn);
}

// Caller holds the lock. RFC 1122 4.2.3.5
static uint8_t retry_limit_locked(const tcp_conn* conn) {
    if (conn->state == tcp_state::syn_sent || conn->state == tcp_state::syn_rcvd) {
        return SYN_RETRIES;
    }

    return conn->orphaned ? ORPHAN_RETRIES : DATA_RETRIES;
}

// Caller holds the lock. A timeout with data outstanding is a loss (RFC 5681 3.1)
static packet* retransmit_oldest_locked(tcp_conn* conn) {
    if (conn->sent.empty()) {
        return nullptr;
    }

    enter_loss_locked(conn);
    return rebuild_oldest_locked(conn);
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
    packet* rebuilt = nullptr;

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
            src = snapshot_source(conn);
            bool window_closed = !seq_lt(conn->snd_nxt, conn->snd_una + conn->snd_wnd);
            bool peer_silent = now_ns() - conn->peer_acked_ns > TIMEOUT_MAX_NS;
            send_action exhausted = conn->sent.empty() ? send_action::give_up : send_action::reset;

            if (kind == timer_kind::orphan) {
                action = send_action::reset;
            } else if (kind == timer_kind::probe && !window_closed) {
                action = send_action::push;
            } else if (kind == timer_kind::probe) {
                if (conn->unanswered_probes >= retry_limit_locked(conn)) {
                    action = send_action::reset;
                } else {
                    conn->unanswered_probes++;
                    back_off_locked(conn);
                    mark_ack_sent_locked(conn);
                    action = send_action::probe;
                }
            } else if (conn->snd_wnd == 0 && !conn->orphaned && is_synchronized(conn->state)) {
                // RFC 1122 4.2.2.17: into a closed window, retransmissions are probes
                if (peer_silent) {
                    action = exhausted;
                } else {
                    back_off_locked(conn);
                    rebuilt = retransmit_oldest_locked(conn);
                    arm_send_timer_locked(conn, timer_kind::rto);
                    action = send_action::retransmit;
                }
            } else if (conn->retransmits >= retry_limit_locked(conn)) {
                action = exhausted;
            } else {
                conn->retransmits++;
                if (conn->retransmits >= DELIVERY_PROBLEM_RETRIES && conn->soft_error == resource::OK) {
                    conn->soft_error = resource::ERR_TIMEDOUT;
                }

                back_off_locked(conn);
                rebuilt = retransmit_oldest_locked(conn);
                arm_send_timer_locked(conn, timer_kind::rto);
                action = send_action::retransmit;
            }

            if (action == send_action::retransmit && !rebuilt) {
                conn->total_retransmits++;
            } else if (action == send_action::reset || action == send_action::give_up) {
                conn->state = tcp_state::closed;
                conn->pending_error = conn->soft_error != resource::OK ? conn->soft_error : resource::ERR_TIMEDOUT;
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

        if (rebuilt) {
            (void)transmit_segment(rebuilt, src.iface, src.key);
        } else {
            (void)retransmit(state, src);
        }
    } else if (action == send_action::probe || action == send_action::push) {
        if (action == send_action::probe) {
            (void)send_probe(src);
        }

        (void)output(conn);
    }

    finish_timer_callback(conn);
}

} // namespace tcp
} // namespace net
