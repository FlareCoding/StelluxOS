#include "net/tcp/listen.h"
#include "net/tcp/input.h"
#include "net/tcp/output.h"
#include "net/tcp/timers.h"
#include "net/tcp/info.h"
#include "net/tcp/rtt.h"
#include "net/tcp/seq.h"
#include "net/net.h"
#include "net/interface.h"
#include "net/byteorder.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

// What a SYN-ACK is built from, copied out under the request lock so the
// segment is sent with no lock held
struct synack_fields {
    interface*  iface;
    tuple       key;
    uint32_t    iss;
    uint32_t    irs;
    tcp_options opts;
};

// What the connection inherits from its request, copied out the same way
struct negotiated_fields {
    uint32_t iss;
    uint32_t irs;
    uint32_t ts_recent;
    uint32_t ts_offset;
    uint16_t peer_mss;
    uint8_t  snd_wscale;
    uint8_t  rcv_wscale;
    bool     wscale_ok;
    bool     sack_ok;
    bool     ts_ok;
    bool     ecn_ok;
};

static tcp_listener* g_listeners[MAX_LISTENERS];
static sync::spinlock g_listeners_lock = sync::SPINLOCK_INIT;

static void on_request_timer(timer::deadline_timer* timer);

static void release_record(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

static int32_t drop(interface* iface, packet* pkt, int32_t rc) {
    iface->record_packet_dropped();
    packet::free(pkt);
    return rc;
}

// Caller holds the request lock
static synack_fields synack_fields_locked(const tcp_request* request) {
    synack_fields fields = {};
    fields.iface = request->iface;
    fields.key = request->key;
    fields.iss = request->iss;
    fields.irs = request->irs;
    fields.opts.mss = local_mss(request->iface);
    fields.opts.sack_permitted = request->sack_ok;
    fields.opts.has_timestamps = request->ts_ok;
    fields.opts.ts_val = timestamp_value(request->ts_offset);
    fields.opts.ts_ecr = request->ts_recent;
    fields.opts.window_scale = request->wscale_ok ? request->rcv_wscale : WINDOW_SCALE_NONE;

    return fields;
}

static int32_t send_synack(const synack_fields& fields) {
    return send_segment(fields.iface, fields.key, FLAG_SYN | FLAG_ACK, fields.iss, fields.irs + 1,
                        window_field(RCV_WND_INITIAL, 0), fields.opts);
}

// Caller holds the request lock
static void arm_request_timer_locked(tcp_request* request) {
    uint64_t backoff = TIMEOUT_INIT_NS << request->retransmits;
    request->timer_deadline_ns = now_ns() + (backoff > TIMEOUT_MAX_NS ? TIMEOUT_MAX_NS : backoff);
    request->timer_armed = true;
    arm_timer(request, &request->timer, request->timer_deadline_ns);
}

static tcp_request* alloc_request(const tuple& key, interface* iface, tcp_listener* listener) {
    tcp_request* request = heap::ualloc_new<tcp_request>();
    if (!request) {
        return nullptr;
    }

    request->kind = record_kind::request;
    request->key = key;
    request->iface = iface;
    request->lock = sync::SPINLOCK_INIT;
    timer::init_deadline_timer(&request->timer, on_request_timer);
    listener->add_ref();
    request->listener = listener;

    return request;
}

// RFC 9293 3.10.7.2: the options a SYN offers decide what the connection will use
static void negotiate(tcp_request* request, const tcp_header* hdr, const tcp_options& opts) {
    request->irs = ntohl(hdr->seq);
    request->iss = initial_sequence(request->key);
    request->peer_mss = opts.mss != MSS_NONE ? opts.mss : DEFAULT_MSS;
    request->wscale_ok = opts.window_scale != WINDOW_SCALE_NONE;
    request->snd_wscale = request->wscale_ok ? opts.window_scale : 0;
    request->rcv_wscale = request->wscale_ok ? receive_window_scale() : 0;
    request->sack_ok = opts.sack_permitted;
    request->ts_ok = opts.has_timestamps;
    request->ts_recent = opts.ts_val;
    request->ts_offset = timestamp_offset(request->key);
}

static void discard_request_count(tcp_listener* listener) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        listener->request_count--;
    });
}

static void discard_request(tcp_request* request) {
    if (remove(request) == OK) {
        discard_request_count(request->listener);
    }
}

static void on_request_timer(timer::deadline_timer* timer) {
    tcp_request* request = timer::owner_of<tcp_request, &tcp_request::timer>(timer);
    bool fire = false;
    bool give_up = false;
    synack_fields fields = {};

    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        if (request->timer_armed) {
            if (now_ns() < request->timer_deadline_ns) {
                arm_timer(request, &request->timer, request->timer_deadline_ns);
                finish_timer_callback(request);
                return;
            }

            fire = true;
            request->timer_armed = false;
            if (request->retransmits >= SYNACK_RETRIES) {
                give_up = true;
            } else {
                request->retransmits++;
                fields = synack_fields_locked(request);
                arm_request_timer_locked(request);
            }
        }
    });

    if (fire && give_up) {
        discard_request(request);
    } else if (fire) {
        increment(counter::retransmits);
        (void)send_synack(fields);
    }

    finish_timer_callback(request);
}

// Caller holds g_listeners_lock
static bool listener_conflicts_locked(const endpoint& local) {
    for (size_t i = 0; i < MAX_LISTENERS; i++) {
        if (g_listeners[i] && endpoints_conflict(g_listeners[i]->local, local)) {
            return true;
        }
    }

    return false;
}

static void release_listener(tcp_listener* listener) {
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }
}

void tcp_listener::ref_destroy(tcp_listener* self) {
    heap::ufree_delete(self);
}

bool endpoints_conflict(const endpoint& a, const endpoint& b) {
    if (a.port != b.port) {
        return false;
    }

    if (a.iface && b.iface && a.iface != b.iface) {
        return false;
    }

    if (a.addr == b.addr) {
        return true;
    }

    bool one_is_wildcard = a.addr.is_unspecified() || b.addr.is_unspecified();
    return one_is_wildcard && !(a.reuseaddr && b.reuseaddr);
}

bool listener_conflicts(const endpoint& local) {
    bool conflicts = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        conflicts = listener_conflicts_locked(local);
    });

    return conflicts;
}

tcp_listener* alloc_listener(const endpoint& local) {
    tcp_listener* listener = heap::ualloc_new<tcp_listener>();
    if (!listener) {
        return nullptr;
    }

    listener->local = local;
    listener->lock = sync::SPINLOCK_INIT;
    listener->accept_queue.init();
    listener->accept_wq.init();

    return listener;
}

int32_t listener_insert(tcp_listener* listener) {
    int32_t rc = ERR_FULL;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        if (listener_conflicts_locked(listener->local)) {
            rc = ERR_IN_USE;
        } else {
            for (size_t i = 0; i < MAX_LISTENERS; i++) {
                if (!g_listeners[i]) {
                    listener->add_ref();
                    g_listeners[i] = listener;
                    rc = OK;
                    break;
                }
            }
        }
    });

    return rc;
}

int32_t listener_remove(tcp_listener* listener) {
    bool removed = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            if (g_listeners[i] == listener) {
                g_listeners[i] = nullptr;
                removed = true;
                break;
            }
        }
    });

    if (!removed) {
        return ERR_NOT_FOUND;
    }

    release_listener(listener);
    return OK;
}

rc::strong_ref<tcp_listener> listener_lookup(const ipv4::ipv4_addr& local_addr, uint16_t port,
                                             interface* iface) {
    tcp_listener* found = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            tcp_listener* candidate = g_listeners[i];
            if (!candidate || candidate->local.port != port) {
                continue;
            }

            if (candidate->local.iface && candidate->local.iface != iface) {
                continue;
            }

            if (candidate->local.addr == local_addr) {
                found = candidate;
                break;
            }

            if (candidate->local.addr.is_unspecified()) {
                found = candidate;
            }
        }

        if (found) {
            found->add_ref();
        }
    });

    return rc::strong_ref<tcp_listener>::adopt(found);
}

bool is_listener_port(uint16_t port) {
    bool taken = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);
        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            if (g_listeners[i] && g_listeners[i]->local.port == port) {
                taken = true;
                break;
            }
        }
    });

    return taken;
}

size_t collect_listeners(tcp_listener** out, size_t max, size_t* total) {
    size_t written = 0;
    *total = 0;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_listeners_lock);

        for (size_t i = 0; i < MAX_LISTENERS; i++) {
            if (!g_listeners[i]) {
                continue;
            }

            (*total)++;

            if (written < max) {
                g_listeners[i]->add_ref();
                out[written++] = g_listeners[i];
            }
        }
    });

    return written;
}

static void retire_request(tcp_request* request) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        request->timer_armed = false;
    });

    disarm_timer(request, &request->timer);
}

static void rearm_request(tcp_request* request) {
    retire_request(request);
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        arm_request_timer_locked(request);
    });
}

void listener_close(tcp_listener* listener) {
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        listener->closed = true;
    });

    (void)listener_remove(listener);

    while (rc::strong_ref<record> rec = remove_one_request_of(listener)) {
        retire_request(static_cast<tcp_request*>(rec.ptr()));
    }

    while (rc::strong_ref<tcp_conn> conn = pop_accepted(listener)) {
        abort_connection(conn.ptr());
    }

    RUN_ELEVATED(sync::wake_all(listener->accept_wq));
}

rc::strong_ref<tcp_conn> pop_accepted(tcp_listener* listener) {
    tcp_conn* conn = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        conn = listener->accept_queue.pop_front();
        if (conn) {
            listener->accept_count--;
        }
    });

    return rc::strong_ref<tcp_conn>::adopt(conn);
}

// Caller holds the request lock
static negotiated_fields negotiated_fields_locked(const tcp_request* request) {
    negotiated_fields fields = {};
    fields.iss = request->iss;
    fields.irs = request->irs;
    fields.ts_recent = request->ts_recent;
    fields.ts_offset = request->ts_offset;
    fields.peer_mss = request->peer_mss;
    fields.snd_wscale = request->snd_wscale;
    fields.rcv_wscale = request->rcv_wscale;
    fields.wscale_ok = request->wscale_ok;
    fields.sack_ok = request->sack_ok;
    fields.ts_ok = request->ts_ok;
    fields.ecn_ok = request->ecn_ok;

    return fields;
}

// RFC 9293 3.10.7.4: the only place a passive connection gets its sequence
// variables, from the request and the ACK that completed it
static void init_from_request(tcp_conn* conn, const negotiated_fields& fields, const tcp_header* hdr,
                              const tcp_options& opts) {
    conn->state = tcp_state::established;
    conn->passive = true;
    conn->iss = fields.iss;
    conn->irs = fields.irs;
    conn->snd_una = fields.iss + 1;
    conn->snd_nxt = fields.iss + 1;
    conn->snd_wnd = uint32_t{ntohs(hdr->window)} << fields.snd_wscale;
    conn->snd_wl1 = ntohl(hdr->seq);
    conn->snd_wl2 = ntohl(hdr->ack);
    conn->max_snd_wnd = conn->snd_wnd;
    conn->snd_mss = fields.peer_mss < local_mss(conn->iface) ? fields.peer_mss : local_mss(conn->iface);
    conn->snd_wscale = fields.snd_wscale;
    conn->rcv_nxt = fields.irs + 1;
    conn->rcv_wnd = RCV_WND_INITIAL;
    conn->rcv_adv = conn->rcv_nxt + RCV_WND_INITIAL;
    conn->rcv_acked = conn->rcv_nxt;
    conn->rcv_mss = fields.peer_mss;
    conn->rcv_wscale = fields.rcv_wscale;
    conn->wscale_ok = fields.wscale_ok;
    configure_send_path_locked(conn);
    conn->sack_ok = fields.sack_ok;
    conn->ts_ok = fields.ts_ok;
    conn->ecn_ok = fields.ecn_ok;
    conn->ts_recent = fields.ts_ok && opts.has_timestamps ? opts.ts_val : fields.ts_recent;
    conn->ts_recent_age_ns = now_ns();
    conn->ts_offset = fields.ts_offset;

    uint64_t rtt_ns = rtt_from_echo_locked(conn, opts.has_timestamps ? opts.ts_ecr : 0);
    if (rtt_ns != 0) {
        take_rtt_sample_locked(conn, rtt_ns);
    }
}

static int32_t send_request_ack(const tcp_request* request, const negotiated_fields& fields) {
    return send_segment(request->iface, request->key, FLAG_ACK, fields.iss + 1, fields.irs + 1,
                        window_field(RCV_WND_INITIAL, 0), {});
}

static int32_t promote(tcp_request* request, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    interface* iface = pkt->iface();
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);

    negotiated_fields fields;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        fields = negotiated_fields_locked(request);
    });

    if (ack != fields.iss + 1) {
        packet::free(pkt);
        return send_segment(iface, request->key, FLAG_RST, ack, 0, 0, {});
    }

    bool paws_reject = fields.ts_ok && opts.has_timestamps && seq_lt(opts.ts_val, fields.ts_recent);
    if (seq != fields.irs + 1 || paws_reject) {
        packet::free(pkt);
        return send_request_ack(request, fields);
    }

    tcp_listener* listener = request->listener;
    bool closed = false;
    bool full = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        closed = listener->closed;
        full = listener->accept_count >= listener->backlog;
    });

    // Nobody will ever accept from a closed listener, so the peer learns it now.
    // A full queue or a failed allocation keeps the request, and the next ACK
    // or SYN-ACK retransmission tries again.
    if (closed) {
        retire_request(request);
        discard_request(request);
        packet::free(pkt);
        return send_segment(iface, request->key, FLAG_RST, ack, 0, 0, {});
    }

    if (full) {
        return drop(iface, pkt, OK);
    }

    tcp_conn* conn = alloc_conn(request->key, iface);
    if (!conn) {
        return drop(iface, pkt, ERR_NO_MEMORY);
    }

    init_from_request(conn, fields, hdr, opts);

    // The timer must not give the request up while this ACK is claiming it, and
    // winning the replace is what makes this ACK the one that accounts for it.
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        request->timer_armed = false;
    });

    int32_t rc = replace(request, conn);
    if (rc == ERR_FULL) {
        release_record(conn);
        rearm_request(request);

        return drop(iface, pkt, rc);
    }

    // Another ACK or a close took the request first; only a peer left with nothing is reset
    if (rc != OK) {
        release_record(conn);
        packet::free(pkt);

        return lookup(request->key) ? OK : send_segment(iface, request->key, FLAG_RST, ack, 0, 0, {});
    }

    retire_request(request);

    bool queued = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        listener->request_count--;
        if (!listener->closed && listener->accept_count < listener->backlog) {
            listener->accept_count++;
            conn->add_ref();
            listener->accept_queue.push_back(conn);
            sync::wake_all(listener->accept_wq);
            queued = true;
        }
    });

    if (!queued) {
        abort_connection(conn);
    }

    release_record(conn);
    packet::free(pkt);

    return OK;
}

int32_t listen_input(tcp_listener* listener, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    interface* iface = pkt->iface();
    const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());
    tuple key = {ip->dst, ip->src, ntohs(hdr->dst_port), ntohs(hdr->src_port)};

    bool room = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        if (!listener->closed && listener->request_count < listener->backlog &&
            listener->accept_count < listener->backlog) {
            listener->request_count++;
            room = true;
        }
    });

    if (!room) {
        increment(counter::listen_drops);
        return drop(iface, pkt, OK);
    }

    tcp_request* request = alloc_request(key, iface, listener);
    if (!request) {
        discard_request_count(listener);
        increment(counter::listen_drops);

        return drop(iface, pkt, ERR_NO_MEMORY);
    }

    negotiate(request, hdr, opts);

    int32_t rc = insert(request);
    if (rc != OK) {
        discard_request_count(listener);
        release_record(request);
        increment(counter::listen_drops);

        return drop(iface, pkt, rc);
    }

    synack_fields fields;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        fields = synack_fields_locked(request);
        arm_request_timer_locked(request);
    });

    // A close that ran since the first check either found this request in the
    // table and retired it, or is seen here and the request retires itself.
    bool closed = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        closed = listener->closed;
    });

    if (closed) {
        (void)remove(request);
        retire_request(request);
        release_record(request);

        return drop(iface, pkt, OK);
    }

    release_record(request);
    packet::free(pkt);

    return send_synack(fields);
}

// RFC 9293 3.10.7.4 with RFC 5961 3.2: a reset at exactly the next expected
// sequence returns the request to LISTEN, one elsewhere in the window is
// challenged, one outside it is ignored
static int32_t request_reset(tcp_request* request, packet* pkt, const tcp_header* hdr) {
    negotiated_fields fields;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        fields = negotiated_fields_locked(request);
    });

    uint32_t seq = ntohl(hdr->seq);
    uint32_t rcv_nxt = fields.irs + 1;
    packet::free(pkt);

    if (seq == rcv_nxt) {
        retire_request(request);
        discard_request(request);
        return OK;
    }

    if (seq_between(seq, rcv_nxt, rcv_nxt + RCV_WND_INITIAL - 1) && take_challenge_ack()) {
        return send_request_ack(request, fields);
    }

    return OK;
}

int32_t request_input(tcp_request* request, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    if (hdr->flags & FLAG_RST) {
        return request_reset(request, pkt, hdr);
    }

    if ((hdr->flags & (FLAG_SYN | FLAG_ACK)) == FLAG_SYN) {
        synack_fields fields;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(request->lock);
            fields = synack_fields_locked(request);
        });

        packet::free(pkt);
        increment(counter::retransmits);

        return send_synack(fields);
    }

    if (hdr->flags & FLAG_ACK) {
        return promote(request, pkt, hdr, opts);
    }

    packet::free(pkt);
    return OK;
}

} // namespace tcp
} // namespace net
