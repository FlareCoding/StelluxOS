#include "net/tcp/timewait.h"
#include "net/tcp/output.h"
#include "net/tcp/timers.h"
#include "net/tcp/seq.h"
#include "net/net.h"
#include "net/byteorder.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

static void on_timewait_timer(timer::deadline_timer* timer);

static void release_record(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

static tcp_timewait* alloc_timewait(const tuple& key, interface* iface) {
    tcp_timewait* tw = heap::ualloc_new<tcp_timewait>();
    if (!tw) {
        return nullptr;
    }

    tw->kind = record_kind::timewait;
    tw->key = key;
    tw->iface = iface;
    tw->lock = sync::SPINLOCK_INIT;
    timer::init_deadline_timer(&tw->timer, on_timewait_timer);

    return tw;
}

static void on_timewait_timer(timer::deadline_timer* timer) {
    tcp_timewait* tw = timer::owner_of<tcp_timewait, &tcp_timewait::timer>(timer);
    bool expired = false;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(tw->lock);
        if (tw->timer_armed) {
            if (now_ns() < tw->timer_deadline_ns) {
                arm_timer(tw, &tw->timer, tw->timer_deadline_ns);
                finish_timer_callback(tw);
                return;
            }

            tw->timer_armed = false;
            expired = true;
        }
    });

    if (expired) {
        (void)remove(tw);
    }

    finish_timer_callback(tw);
}

void enter_time_wait(tcp_conn* conn) {
    tcp_timewait* tw = alloc_timewait(conn->key, conn->iface);

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (tw) {
            tw->snd_nxt = conn->snd_nxt;
            tw->rcv_nxt = conn->rcv_nxt;
            tw->ts_recent = conn->ts_recent;
            tw->ts_recent_age_ns = conn->ts_recent_age_ns;
            tw->ts_ok = conn->ts_ok;
        }

        conn->state = tcp_state::closed;
        conn->send_timer_kind = timer_kind::none;
    });

    if (tw && replace(conn, tw) == OK) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(tw->lock);
            tw->timer_deadline_ns = now_ns() + TIMEWAIT_LEN_NS;
            tw->timer_armed = true;
            arm_timer(tw, &tw->timer, tw->timer_deadline_ns);
        });
    }

    if (tw) {
        release_record(tw);
    }

    retire_connection(conn);
}

int32_t timewait_input(tcp_timewait* tw, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    if (hdr->flags & FLAG_RST) {
        packet::free(pkt);
        return OK;
    }

    uint32_t seq = ntohl(hdr->seq);
    bool reopen = false;
    bool ack_fin = false;
    uint32_t snd_nxt = 0;
    uint32_t rcv_nxt = 0;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(tw->lock);
        if ((hdr->flags & (FLAG_SYN | FLAG_ACK)) == FLAG_SYN) {
            bool newer_stamp = tw->ts_ok && opts.has_timestamps && seq_gt(opts.ts_val, tw->ts_recent);
            reopen = seq_gt(seq, tw->rcv_nxt) || newer_stamp;

            if (reopen) {
                tw->timer_armed = false;
            }
        } else if (hdr->flags & FLAG_FIN) {
            tw->timer_deadline_ns = now_ns() + TIMEWAIT_LEN_NS;
            snd_nxt = tw->snd_nxt;
            rcv_nxt = tw->rcv_nxt;
            ack_fin = true;
        }
    });

    if (reopen) {
        disarm_timer(tw, &tw->timer);
        (void)remove(tw);

        return TIMEWAIT_REOPEN;
    }

    packet::free(pkt);
    if (ack_fin) {
        return send_segment(tw->iface, tw->key, FLAG_ACK, snd_nxt, rcv_nxt, 0, {});
    }

    return OK;
}

} // namespace tcp
} // namespace net
