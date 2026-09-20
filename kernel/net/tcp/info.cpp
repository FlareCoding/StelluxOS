#include "net/tcp/info.h"
#include "net/tcp/conn.h"
#include "net/tcp/timewait.h"
#include "net/net.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

namespace net {
namespace tcp {

static sync::atomic<uint64_t> g_counters[static_cast<size_t>(counter::count)];

static void release_record(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

static void release_listener(tcp_listener* listener) {
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }
}

static uint32_t to_host_order(const ipv4::ipv4_addr& addr) {
    return (static_cast<uint32_t>(addr.bytes[0]) << 24) | (static_cast<uint32_t>(addr.bytes[1]) << 16) |
           (static_cast<uint32_t>(addr.bytes[2]) << 8) | addr.bytes[3];
}

static uint32_t remaining_ms(uint64_t deadline_ns) {
    uint64_t now = now_ns();
    return deadline_ns > now ? static_cast<uint32_t>((deadline_ns - now) / 1000000) : 0;
}

static void describe_iface(const interface* iface, tcp_record* out) {
    if (iface) {
        string::memcpy(out->iface, iface->name(), IFACE_NAME_MAX);
        out->iface[IFACE_NAME_MAX - 1] = '\0';
    }
}

static void describe_tuple(const tuple& key, tcp_record* out) {
    out->local_addr = to_host_order(key.local_addr);
    out->remote_addr = to_host_order(key.remote_addr);
    out->local_port = key.local_port;
    out->remote_port = key.remote_port;
}

// Caller holds the request lock
static void describe_request_locked(const tcp_request* request, tcp_record* out) {
    out->kind = INFO_KIND_REQUEST;
    out->state = static_cast<uint8_t>(tcp_state::syn_rcvd);
    out->snd_una = request->iss;
    out->snd_nxt = request->iss + 1;
    out->rcv_nxt = request->irs + 1;
    out->snd_mss = request->peer_mss;
    out->snd_wscale = request->snd_wscale;
    out->rcv_wscale = request->rcv_wscale;
    out->retransmits = request->retransmits;
    out->flags = (request->ts_ok ? INFO_TIMESTAMPS : 0) | (request->sack_ok ? INFO_SACK : 0);
    if (request->timer_armed) {
        out->timer_kind = INFO_TIMER_RTO;
        out->timer_ms = remaining_ms(request->timer_deadline_ns);
    }
}

// Caller holds the connection lock
static void describe_conn_locked(const tcp_conn* conn, tcp_record* out) {
    out->kind = INFO_KIND_CONNECTION;
    out->state = static_cast<uint8_t>(conn->state);
    out->snd_una = conn->snd_una;
    out->snd_nxt = conn->snd_nxt;
    out->rcv_nxt = conn->rcv_nxt;
    out->snd_wnd = conn->snd_wnd;
    out->rcv_wnd = conn->rcv_wnd;
    out->snd_mss = conn->snd_mss;
    out->snd_wscale = conn->snd_wscale;
    out->rcv_wscale = conn->rcv_wscale;
    out->retransmits = conn->retransmits;
    out->error = conn->pending_error;
    out->flags = (conn->ts_ok ? INFO_TIMESTAMPS : 0) | (conn->sack_ok ? INFO_SACK : 0) |
                 (conn->wscale_ok ? INFO_WINDOW_SCALE : 0) | (conn->orphaned ? INFO_ORPHANED : 0) |
                 (conn->fin_sent ? INFO_FIN_SENT : 0) | (conn->fin_rcvd ? INFO_FIN_RCVD : 0);

    switch (conn->send_timer_kind) {
    case timer_kind::rto:
        out->timer_kind = INFO_TIMER_RTO;
        out->timer_ms = remaining_ms(conn->send_timer_deadline_ns);
        break;
    case timer_kind::orphan:
        out->timer_kind = INFO_TIMER_ORPHAN;
        out->timer_ms = remaining_ms(conn->send_timer_deadline_ns);
        break;
    case timer_kind::probe:
        out->timer_kind = INFO_TIMER_PROBE;
        out->timer_ms = remaining_ms(conn->send_timer_deadline_ns);
        break;
    default:
        break;
    }
}

// Caller holds the record lock
static void describe_timewait_locked(const tcp_timewait* tw, tcp_record* out) {
    out->kind = INFO_KIND_TIMEWAIT;
    out->state = static_cast<uint8_t>(tcp_state::time_wait);
    out->snd_una = tw->snd_nxt;
    out->snd_nxt = tw->snd_nxt;
    out->rcv_nxt = tw->rcv_nxt;
    out->flags = tw->ts_ok ? INFO_TIMESTAMPS : 0;
    if (tw->timer_armed) {
        out->timer_kind = INFO_TIMER_TIMEWAIT;
        out->timer_ms = remaining_ms(tw->timer_deadline_ns);
    }
}

void increment(counter which) {
    g_counters[static_cast<size_t>(which)].fetch_add_relaxed(1);
}

void describe_counters(tcp_counters* out) {
    *out = {};
    out->segments_in = g_counters[static_cast<size_t>(counter::segments_in)].load_relaxed();
    out->segments_out = g_counters[static_cast<size_t>(counter::segments_out)].load_relaxed();
    out->retransmits = g_counters[static_cast<size_t>(counter::retransmits)].load_relaxed();
    out->rsts_sent = g_counters[static_cast<size_t>(counter::rsts_sent)].load_relaxed();
    out->rsts_received = g_counters[static_cast<size_t>(counter::rsts_received)].load_relaxed();
    out->checksum_failures = g_counters[static_cast<size_t>(counter::checksum_failures)].load_relaxed();
    out->listen_drops = g_counters[static_cast<size_t>(counter::listen_drops)].load_relaxed();
    out->paws_drops = g_counters[static_cast<size_t>(counter::paws_drops)].load_relaxed();
    out->challenge_acks = g_counters[static_cast<size_t>(counter::challenge_acks)].load_relaxed();
    out->requests = static_cast<uint32_t>(record_count(record_kind::request));
    out->connections = static_cast<uint32_t>(record_count(record_kind::connection));
    out->timewaits = static_cast<uint32_t>(record_count(record_kind::timewait));

    size_t listeners = 0;
    (void)collect_listeners(nullptr, 0, &listeners);
    out->listeners = static_cast<uint32_t>(listeners);
}

void describe_listener(tcp_listener* listener, tcp_record* out) {
    *out = {};
    out->kind = INFO_KIND_LISTENER;
    out->state = static_cast<uint8_t>(tcp_state::listen);
    out->local_addr = to_host_order(listener->local.addr);
    out->local_port = listener->local.port;
    describe_iface(listener->local.iface, out);

    RUN_ELEVATED({
        sync::irq_lock_guard guard(listener->lock);
        out->backlog = listener->backlog;
        out->requests = listener->request_count;
        out->accepted = listener->accept_count;
    });
}

void describe_record(record* rec, tcp_record* out) {
    *out = {};
    describe_tuple(rec->key, out);
    describe_iface(rec->iface, out);

    RUN_ELEVATED({
        sync::irq_lock_guard guard(rec->lock);
        switch (rec->kind) {
        case record_kind::request:
            describe_request_locked(static_cast<const tcp_request*>(rec), out);
            break;
        case record_kind::connection:
            describe_conn_locked(static_cast<const tcp_conn*>(rec), out);
            break;
        case record_kind::timewait:
            describe_timewait_locked(static_cast<const tcp_timewait*>(rec), out);
            break;
        }
    });
}

__PRIVILEGED_CODE static int32_t write_record(uint64_t records, size_t index, const tcp_record* entry) {
    void* dest = reinterpret_cast<void*>(records + index * sizeof(tcp_record));
    return mm::uaccess::copy_to_user(dest, entry, sizeof(tcp_record)) == mm::uaccess::OK ? OK : ERR_INVALID;
}

__PRIVILEGED_CODE int32_t query_info(uint64_t user_info) {
    tcp_info info;
    if (mm::uaccess::copy_from_user(&info, reinterpret_cast<const void*>(user_info), sizeof(info)) != mm::uaccess::OK) {
        return ERR_INVALID;
    }

    size_t capacity = info.capacity < MAX_INFO_RECORDS ? info.capacity : MAX_INFO_RECORDS;
    record** records = nullptr;
    
    if (capacity > 0) {
        records = static_cast<record**>(heap::ualloc(capacity * sizeof(record*)));
        if (!records) {
            return ERR_NO_MEMORY;
        }
    }

    tcp_listener* listeners[MAX_LISTENERS];
    size_t listener_total = 0;
    size_t record_total = 0;
    size_t listener_count = collect_listeners(listeners, capacity < MAX_LISTENERS ? capacity : MAX_LISTENERS, &listener_total);
    size_t record_count = collect_records(records, capacity - listener_count, &record_total);

    int32_t rc = OK;
    tcp_record entry;
    for (size_t i = 0; i < listener_count; i++) {
        describe_listener(listeners[i], &entry);
        release_listener(listeners[i]);
        
        if (rc == OK) {
            rc = write_record(info.records, i, &entry);
        }
    }

    for (size_t i = 0; i < record_count; i++) {
        describe_record(records[i], &entry);
        release_record(records[i]);
        if (rc == OK) {
            rc = write_record(info.records, listener_count + i, &entry);
        }
    }

    if (records) {
        heap::ufree(records);
    }

    if (rc != OK) {
        return rc;
    }

    info.count = static_cast<uint32_t>(listener_count + record_count);
    info.total = static_cast<uint32_t>(listener_total + record_total);
    describe_counters(&info.counters);

    return mm::uaccess::copy_to_user(reinterpret_cast<void*>(user_info), &info, sizeof(info)) == mm::uaccess::OK
               ? OK : ERR_INVALID;
}

} // namespace tcp
} // namespace net
