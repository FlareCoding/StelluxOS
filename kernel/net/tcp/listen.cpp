#include "net/tcp/listen.h"
#include "net/tcp/output.h"
#include "net/tcp/timers.h"
#include "net/net.h"
#include "net/interface.h"
#include "net/byteorder.h"
#include "sync/spinlock.h"
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

static uint16_t local_mss(const interface* iface) {
    return static_cast<uint16_t>(iface->mtu() - ipv4::HEADER_LEN - HEADER_LEN);
}

static uint16_t advertised_window(uint32_t rcv_wnd) {
    return static_cast<uint16_t>(rcv_wnd > 0xFFFF ? 0xFFFF : rcv_wnd);
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
                        advertised_window(RCV_BUF_INITIAL), fields.opts);
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

void listener_close(tcp_listener* listener) {
    (void)listener_remove(listener);

    while (rc::strong_ref<record> rec = remove_one_request_of(listener)) {
        tcp_request* request = static_cast<tcp_request*>(rec.ptr());
        RUN_ELEVATED({
            sync::irq_lock_guard guard(request->lock);
            request->timer_armed = false;
        });

        disarm_timer(request, &request->timer);
    }
}

int32_t listen_input(tcp_listener* listener, packet* pkt, const tcp_header* hdr, const tcp_options& opts) {
    interface* iface = pkt->iface();
    const ipv4::ipv4_header* ip = reinterpret_cast<const ipv4::ipv4_header*>(pkt->network_header());
    tuple key = {ip->dst, ip->src, ntohs(hdr->dst_port), ntohs(hdr->src_port)};

    bool room = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        if (listener->request_count < listener->backlog && listener->accept_count < listener->backlog) {
            listener->request_count++;
            room = true;
        }
    });

    if (!room) {
        return drop(iface, pkt, OK);
    }

    tcp_request* request = alloc_request(key, iface, listener);
    if (!request) {
        discard_request_count(listener);
        return drop(iface, pkt, ERR_NO_MEMORY);
    }

    negotiate(request, hdr, opts);

    int32_t rc = insert(request);
    if (rc != OK) {
        discard_request_count(listener);
        release_record(request);
        return drop(iface, pkt, rc);
    }

    synack_fields fields;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        fields = synack_fields_locked(request);
        arm_request_timer_locked(request);
    });

    release_record(request);
    packet::free(pkt);

    return send_synack(fields);
}

int32_t request_input(tcp_request* request, packet* pkt, const tcp_header* hdr, const tcp_options&) {
    bool retransmitted_syn = (hdr->flags & (FLAG_SYN | FLAG_ACK | FLAG_RST)) == FLAG_SYN;
    if (!retransmitted_syn) {
        packet::free(pkt);
        return OK;
    }

    synack_fields fields;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(request->lock);
        fields = synack_fields_locked(request);
    });

    packet::free(pkt);
    return send_synack(fields);
}

} // namespace tcp
} // namespace net
