#include "net/tcp/recovery.h"
#include "net/tcp/seq.h"
#include "net/tcp/wire.h"

namespace net {
namespace tcp {

static void enter_state_locked(tcp_conn* conn, recovery_state state) {
    conn->recovery = state;
    if (conn->congestion->set_state) {
        conn->congestion->set_state(conn, state);
    }
}

static void notify(tcp_conn* conn, congestion_event which) {
    if (conn->congestion->event) {
        conn->congestion->event(conn, which);
    }
}

// Caller holds the lock. RFC 6582 3.2 step 1: only once the acknowledgments
// have passed the point the last recovery was entered at
static bool may_enter_recovery_locked(const tcp_conn* conn) {
    return conn->recovery == recovery_state::disorder && conn->dupacks == DUPACK_THRESHOLD &&
           seq_gt(conn->snd_una, conn->high_seq);
}

// Caller holds the lock. RFC 5681 3.2 steps 2 and 3
static void enter_recovery_locked(tcp_conn* conn) {
    conn->prior_cwnd = conn->cwnd;
    conn->prior_ssthresh = conn->ssthresh;
    conn->ssthresh = conn->congestion->ssthresh(conn);
    conn->high_seq = conn->snd_nxt;
    conn->cwnd = conn->ssthresh + DUPACK_THRESHOLD * conn->snd_mss;
    conn->bytes_acked = 0;
    enter_state_locked(conn, recovery_state::recovery);
}

// Caller holds the lock. RFC 6582 3.2 step 3
static void deflate_for_partial_ack_locked(tcp_conn* conn, uint32_t acked_bytes) {
    uint32_t deflated = conn->cwnd > acked_bytes ? conn->cwnd - acked_bytes : 0;
    if (acked_bytes >= conn->snd_mss) {
        deflated += conn->snd_mss;
    }

    conn->cwnd = deflated > conn->snd_mss ? deflated : conn->snd_mss;
}

// Caller holds the lock. RFC 6582 3.2 step 2, the first option
static void finish_recovery_locked(tcp_conn* conn) {
    uint32_t in_flight = conn->snd_nxt - conn->snd_una;
    uint32_t settled = (in_flight > conn->snd_mss ? in_flight : conn->snd_mss) + conn->snd_mss;
    conn->cwnd = settled < conn->ssthresh ? settled : conn->ssthresh;
    conn->bytes_acked = 0;
    enter_state_locked(conn, recovery_state::open);
}

// Caller holds the lock. RFC 5682 3: the timeout was spurious, nothing was lost
static void undo_loss_locked(tcp_conn* conn) {
    conn->cwnd = conn->congestion->undo(conn);
    conn->ssthresh = conn->prior_ssthresh;
    conn->sent.clear_lost_marks();
    conn->frto_step = 0;
    enter_state_locked(conn, recovery_state::open);
}

// Caller holds the lock. RFC 5682 2.2 steps 2 and 3 for an acknowledgment
// that moved SND.UNA: the second one in a row proves the timeout spurious
static recovery_action take_loss_ack_locked(tcp_conn* conn, uint32_t ack) {
    if (conn->frto_step == FRTO_SECOND_ACK) {
        undo_loss_locked(conn);
        return recovery_action::recovered;
    }

    if (seq_lt(ack, conn->high_seq)) {
        if (conn->frto_step == FRTO_FIRST_ACK) {
            conn->frto_step = unsent_bytes(conn) > 0 ? FRTO_SECOND_ACK : 0;
        }

        return recovery_action::none;
    }

    conn->frto_step = 0;
    enter_state_locked(conn, recovery_state::open);
    return recovery_action::recovered;
}

// Caller holds the lock. RFC 5682 2.2 steps 2b and 3b: a duplicate ends the
// test, and after two new segments went out the window must let one lost
// segment go again
static void take_loss_duplicate_locked(tcp_conn* conn) {
    if (conn->frto_step == FRTO_SECOND_ACK) {
        conn->cwnd = 3u * conn->snd_mss;
    }

    conn->frto_step = 0;
}

bool is_duplicate_ack_locked(const tcp_conn* conn, uint8_t flags, size_t payload_len, uint32_t ack,
                             uint32_t window) {
    return conn->snd_nxt != conn->snd_una && payload_len == 0 && !(flags & (FLAG_SYN | FLAG_FIN)) &&
           ack == conn->snd_una && window == conn->snd_wnd;
}

recovery_action take_duplicate_ack_locked(tcp_conn* conn) {
    if (conn->dupacks < 0xFF) {
        conn->dupacks++;
    }

    if (conn->recovery == recovery_state::open) {
        enter_state_locked(conn, recovery_state::disorder);
    }

    if (may_enter_recovery_locked(conn)) {
        enter_recovery_locked(conn);
        return recovery_action::retransmit;
    }

    if (conn->recovery == recovery_state::recovery) {
        conn->cwnd += conn->snd_mss;
    } else if (conn->recovery == recovery_state::loss) {
        take_loss_duplicate_locked(conn);
    }

    return recovery_action::none;
}

recovery_action take_advancing_ack_locked(tcp_conn* conn, uint32_t ack, uint32_t acked_bytes) {
    conn->dupacks = 0;

    switch (conn->recovery) {
    case recovery_state::disorder:
        enter_state_locked(conn, recovery_state::open);
        return recovery_action::none;
    case recovery_state::recovery:
        if (seq_lt(ack, conn->high_seq)) {
            deflate_for_partial_ack_locked(conn, acked_bytes);
            return recovery_action::retransmit;
        }

        finish_recovery_locked(conn);
        return recovery_action::recovered;
    case recovery_state::loss:
        return take_loss_ack_locked(conn, ack);
    default:
        return recovery_action::none;
    }
}

void enter_loss_locked(tcp_conn* conn) {
    bool first_timeout = conn->recovery != recovery_state::loss;
    if (first_timeout) {
        conn->prior_cwnd = conn->cwnd;
        conn->prior_ssthresh = conn->ssthresh;
        conn->ssthresh = conn->congestion->ssthresh(conn);
    }

    conn->frto_step = first_timeout ? FRTO_FIRST_ACK : 0;
    conn->cwnd = conn->snd_mss;
    conn->bytes_acked = 0;
    conn->dupacks = 0;
    conn->high_seq = conn->snd_nxt;
    conn->sent.mark_all_lost();

    notify(conn, congestion_event::loss);
    enter_state_locked(conn, recovery_state::loss);
}

uint32_t pipe_bytes(const tcp_conn* conn) {
    uint32_t outstanding = conn->snd_nxt - conn->snd_una;
    uint32_t lost = static_cast<uint32_t>(conn->sent.lost_bytes());
    return outstanding > lost ? outstanding - lost : 0;
}

uint32_t limited_transmit_bytes(const tcp_conn* conn) {
    if (conn->recovery != recovery_state::disorder) {
        return 0;
    }

    uint32_t segments = conn->dupacks < LIMITED_TRANSMIT_SEGMENTS ? conn->dupacks : LIMITED_TRANSMIT_SEGMENTS;
    return segments * conn->snd_mss;
}

} // namespace tcp
} // namespace net
