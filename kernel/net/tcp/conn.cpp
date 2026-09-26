#include "net/tcp/conn.h"
#include "net/tcp/listen.h"
#include "net/tcp/timewait.h"
#include "net/tcp/socket.h"
#include "net/tcp/wire.h"
#include "net/tcp/output.h"
#include "net/tcp/timers.h"
#include "net/tcp/reassembly.h"
#include "net/tcp/seq.h"
#include "net/net.h"
#include "sync/wait_queue.h"
#include "common/siphash.h"
#include "common/hash.h"
#include "random/random.h"
#include "clock/clock.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"

namespace net {
namespace tcp {

static hash::siphash_key g_tuple_secret;
static hash::siphash_key g_isn_secret;
static hash::siphash_key g_timestamp_secret;
static uint64_t (*g_clock)() = clock::now_ns;

struct record_key_ops {
    using key_type = tuple;

    static key_type key_of(const record& rec) { return rec.key; }
    static uint64_t hash(const key_type& key) { return hash::siphash(&key, sizeof(key), g_tuple_secret); }
    static bool equal(const key_type& a, const key_type& b) { return a == b; }
};

static hashmap::map<record, &record::table_link, record_key_ops> g_table;
static hashmap::bucket g_buckets[TABLE_BUCKETS];
static sync::spinlock g_table_lock = sync::SPINLOCK_INIT;
static size_t g_counts[3];
static uint16_t g_next_ephemeral_port = EPHEMERAL_PORT_MIN;

static size_t capacity_of(record_kind kind) {
    switch (kind) {
    case record_kind::request:
        return MAX_REQUESTS;
    case record_kind::connection:
        return MAX_CONNECTIONS;
    case record_kind::timewait:
        return MAX_TIMEWAIT;
    }

    return 0;
}

static size_t& count_of(record_kind kind) {
    return g_counts[static_cast<size_t>(kind)];
}

static bool is_in_table(const record* rec) {
    return rec->table_link.pprev != nullptr;
}

// Caller holds g_table_lock
static bool is_record_port_locked(uint16_t port) {
    bool taken = false;
    g_table.for_each([&](record& rec) {
        if (rec.key.local_port == port) {
            taken = true;
        }
    });

    return taken;
}

static void release_record(record* rec) {
    if (rec->release()) {
        record::ref_destroy(rec);
    }
}

void record::ref_destroy(record* self) {
    switch (self->kind) {
    case record_kind::request:
        heap::ufree_delete(static_cast<tcp_request*>(self));
        break;
    case record_kind::connection: {
        tcp_conn* conn = static_cast<tcp_conn*>(self);
        if (conn->congestion->release) {
            conn->congestion->release(conn);
        }

        clear_out_of_order_locked(conn);
        conn->rcv_queue.clear();
        conn->snd_queue.clear();
        conn->sent.clear();
        heap::ufree_delete(conn);
        break;
    }
    case record_kind::timewait:
        heap::ufree_delete(static_cast<tcp_timewait*>(self));
        break;
    }
}

static void draw_secret(hash::siphash_key* secret, const char* purpose) {
    if (random::fill(secret, sizeof(*secret)) == random::OK) {
        return;
    }

    log::warn("tcp: no hardware random source, %s secret derived from the clock", purpose);
    secret->k0 = hash::u64(clock::now_ns());
    secret->k1 = hash::u64(~clock::now_ns() ^ hash::ptr(secret));
}

int32_t init_tables() {
    draw_secret(&g_tuple_secret, "table");
    g_table.init(g_buckets, TABLE_BUCKETS);

    return OK;
}

int32_t init_sequence_numbers() {
    draw_secret(&g_isn_secret, "sequence number");
    draw_secret(&g_timestamp_secret, "timestamp");

    return OK;
}

uint32_t initial_sequence(const tuple& key) {
    uint32_t clock_part = static_cast<uint32_t>(now_ns() / ISN_TICK_NS);
    uint32_t tuple_part = static_cast<uint32_t>(hash::siphash(&key, sizeof(key), g_isn_secret));

    return clock_part + tuple_part;
}

uint32_t timestamp_offset(const tuple& key) {
    return static_cast<uint32_t>(hash::siphash(&key, sizeof(key), g_timestamp_secret));
}

uint64_t now_ns() {
    return g_clock();
}

void __dbg_test_set_clock(uint64_t (*fn)()) {
    g_clock = fn ? fn : clock::now_ns;
}

uint32_t timestamp_value(uint32_t offset) {
    return static_cast<uint32_t>(now_ns() / TIMESTAMP_TICK_NS) + offset;
}

void update_receive_window_locked(tcp_conn* conn) {
    size_t room = conn->rcv_queue.free_space();
    uint32_t window = static_cast<uint32_t>(room > conn->ooo_bytes ? room - conn->ooo_bytes : 0);
    window &= ~((1u << conn->rcv_wscale) - 1);

    if (seq_lt(conn->rcv_nxt + window, conn->rcv_adv)) {
        window = conn->rcv_adv - conn->rcv_nxt;
    }

    conn->rcv_wnd = window;
}

void configure_send_path_locked(tcp_conn* conn) {
    conn->cwnd = initial_window(conn->snd_mss);
    conn->ssthresh = SSTHRESH_INFINITE;
    conn->bytes_acked = 0;
    conn->max_in_flight = 0;
    conn->in_flight_window_end = conn->snd_nxt;
    conn->last_data_sent_ns = now_ns();
    conn->cwnd_limited = false;
    conn->recovery = recovery_state::open;
    conn->snd_sml = conn->iss;
    conn->sent.set_cap(conn->snd_queue.limit() * CHUNK_PAYLOAD / conn->snd_mss + SENT_SEGMENT_MARGIN);
    if (conn->congestion->init) {
        conn->congestion->init(conn);
    }
}

void mark_ack_sent_locked(tcp_conn* conn) {
    conn->rcv_acked = conn->rcv_nxt;
    conn->rcv_adv = conn->rcv_nxt + conn->rcv_wnd;
    conn->ack_pending = false;
    conn->ack_timer_armed = false;
    conn->dsack_pending = false;
    if (conn->quick_acks > 0) {
        conn->quick_acks--;
    }
}

uint8_t receive_window_scale() {
    size_t space = MAX_BUF;
    uint8_t scale = 0;
    while (space > 0xFFFF && scale < MAX_WINDOW_SCALE) {
        space >>= 1;
        scale++;
    }

    return scale;
}

tcp_conn* alloc_conn(const tuple& key, interface* iface) {
    tcp_conn* conn = heap::ualloc_new<tcp_conn>();
    if (!conn) {
        return nullptr;
    }

    conn->kind = record_kind::connection;
    conn->key = key;
    conn->iface = iface;
    conn->lock = sync::SPINLOCK_INIT;
    conn->state = tcp_state::closed;
    conn->rcv_queue.init(RCV_CHUNKS_INITIAL);
    conn->rx_wq.init();
    conn->ooo_queue.init();
    conn->snd_queue.init(SND_CHUNKS_INITIAL);
    conn->sent.init(SND_CHUNKS_INITIAL * CHUNK_PAYLOAD / DEFAULT_MSS + SENT_SEGMENT_MARGIN);
    conn->tx_wq.init();
    conn->conn_wq.init();
    conn->quick_acks = MAX_QUICKACKS;
    conn->rto_ns = TIMEOUT_INIT_NS;
    conn->peer_acked_ns = now_ns();
    conn->congestion = default_congestion_ops();
    timer::init_deadline_timer(&conn->send_timer, on_send_timer);
    timer::init_deadline_timer(&conn->ack_timer, on_ack_timer);

    return conn;
}

int32_t open_active(const tuple& key, interface* iface, rc::strong_ref<tcp_conn>* out, const conn_options& options) {
    tcp_conn* conn = alloc_conn(key, iface);
    if (!conn) {
        return ERR_NO_MEMORY;
    }

    apply_options(conn, options);
    conn->state = tcp_state::syn_sent;
    conn->iss = initial_sequence(key);
    conn->snd_una = conn->iss;
    conn->snd_nxt = conn->iss + 1;
    conn->rcv_wnd = RCV_WND_INITIAL;
    conn->rcv_wscale = receive_window_scale();
    conn->ts_offset = timestamp_offset(key);

    int32_t rc = insert(conn);
    if (rc != OK) {
        release_record(conn);
        return rc;
    }

    segment_source src;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        src = snapshot_source(conn);
        arm_send_timer_locked(conn, timer_kind::rto);
    });

    (void)send_syn(src);
    *out = rc::strong_ref<tcp_conn>::adopt(conn);

    return OK;
}

void fail_connection(tcp_conn* conn, int32_t error) {
    bool failed = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (conn->state == tcp_state::syn_sent || conn->state == tcp_state::syn_rcvd) {
            conn->state = tcp_state::closed;
            conn->pending_error = error;
            conn->send_timer_kind = timer_kind::none;
            failed = true;
        }
    });

    if (failed) {
        retire_connection(conn);
    }
}

void retire_connection(tcp_conn* conn) {
    disarm_timer(conn, &conn->send_timer);
    disarm_timer(conn, &conn->ack_timer);
    (void)remove(conn);

    RUN_ELEVATED({
        sync::wake_all(conn->conn_wq);
        sync::wake_all(conn->rx_wq);
        sync::wake_all(conn->tx_wq);
    });
}

void abort_connection(tcp_conn* conn) {
    bool send = false;
    uint32_t seq = 0;
    uint32_t ack = 0;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        if (conn->state != tcp_state::closed) {
            send = conn->state != tcp_state::syn_sent;
            seq = conn->snd_nxt;
            ack = conn->rcv_nxt;
            conn->state = tcp_state::closed;
            conn->send_timer_kind = timer_kind::none;
        }
    });

    if (send) {
        (void)send_segment(conn->iface, conn->key, FLAG_RST | FLAG_ACK, seq, ack, 0, {});
    }

    retire_connection(conn);
}

// Caller holds the lock. RFC 9293 3.10.4
static bool queue_fin_locked(tcp_conn* conn) {
    switch (conn->state) {
    case tcp_state::syn_rcvd:
    case tcp_state::established:
        conn->state = tcp_state::fin_wait_1;
        break;
    case tcp_state::close_wait:
        conn->state = tcp_state::last_ack;
        break;
    default:
        return false;
    }

    conn->fin_pending = true;

    return true;
}

void shutdown_send(tcp_conn* conn) {
    bool send = false;
    RUN_ELEVATED({
        {
            sync::irq_lock_guard guard(conn->lock);
            send = queue_fin_locked(conn);
        }

        sync::wake_all(conn->tx_wq);
    });

    if (send) {
        (void)output(conn);
    }
}

void shutdown_receive(tcp_conn* conn) {
    RUN_ELEVATED({
        {
            sync::irq_lock_guard guard(conn->lock);
            conn->rcv_shutdown = true;
        }

        sync::wake_all(conn->rx_wq);
    });
}

void close_connection(tcp_conn* conn) {
    bool abort = false;
    bool send = false;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(conn->lock);
        conn->owner = nullptr;
        conn->orphaned = true;
        conn->rcv_shutdown = true;

        bool unread = conn->rcv_queue.size() > 0 || !conn->ooo_queue.empty();
        if (conn->state == tcp_state::syn_sent || unread) {
            abort = true;
        } else if (queue_fin_locked(conn)) {
            send = true;
        } else if (conn->state == tcp_state::fin_wait_2) {
            arm_orphan_timer_locked(conn);
        }
    });

    if (abort) {
        abort_connection(conn);
    } else if (send) {
        (void)output(conn);
    }
}

rc::strong_ref<record> lookup(const tuple& key) {
    record* found = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);

        found = g_table.find(key);
        if (found) {
            found->add_ref();
        }
    });

    return rc::strong_ref<record>::adopt(found);
}

int32_t insert(record* rec) {
    int32_t rc = OK;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);

        if (g_table.find(rec->key)) {
            rc = ERR_IN_USE;
        } else if (count_of(rec->kind) >= capacity_of(rec->kind)) {
            rc = ERR_FULL;
        } else {
            rec->add_ref();
            g_table.insert(rec);
            count_of(rec->kind)++;
        }
    });

    return rc;
}

int32_t replace(record* old, record* fresh) {
    int32_t rc = OK;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);
        if (!is_in_table(old) || old->key != fresh->key) {
            rc = ERR_NOT_FOUND;
        } else if (count_of(fresh->kind) >= capacity_of(fresh->kind)) {
            rc = ERR_FULL;
        } else {
            g_table.remove(*old);
            count_of(old->kind)--;
            fresh->add_ref();
            g_table.insert(fresh);
            count_of(fresh->kind)++;
        }
    });

    if (rc == OK) {
        release_record(old);
    }

    return rc;
}

int32_t remove(record* rec) {
    bool removed = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);
        if (is_in_table(rec)) {
            g_table.remove(*rec);
            count_of(rec->kind)--;
            removed = true;
        }
    });

    if (!removed) {
        return ERR_NOT_FOUND;
    }

    release_record(rec);
    return OK;
}

size_t record_count(record_kind kind) {
    size_t count = 0;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);
        count = count_of(kind);
    });

    return count;
}

size_t collect_records(record** out, size_t max, size_t* total) {
    size_t written = 0;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);
        *total = g_table.size();

        g_table.for_each([&](record& rec) {
            if (written < max) {
                rec.add_ref();
                out[written++] = &rec;
            }
        });
    });

    return written;
}

rc::strong_ref<record> remove_one_request_of(const tcp_listener* listener) {
    record* found = nullptr;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);
        g_table.for_each([&](record& rec) {
            if (!found && rec.kind == record_kind::request &&
                static_cast<tcp_request&>(rec).listener == listener) {
                found = &rec;
            }
        });

        if (found) {
            g_table.remove(*found);
            count_of(record_kind::request)--;
        }
    });

    return rc::strong_ref<record>::adopt(found);
}

bool is_local_port_taken(uint16_t port) {
    bool taken = false;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);
        taken = is_record_port_locked(port);
    });

    return taken || is_listener_port(port) || is_socket_port(port);
}

int32_t take_ephemeral_port(uint16_t* out) {
    constexpr uint32_t RANGE = EPHEMERAL_PORT_MAX - EPHEMERAL_PORT_MIN + 1;
    int32_t rc = ERR_FULL;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(g_table_lock);
        for (uint32_t i = 0; i < RANGE; i++) {
            uint16_t port = g_next_ephemeral_port;
            g_next_ephemeral_port = port == EPHEMERAL_PORT_MAX ? EPHEMERAL_PORT_MIN : port + 1;

            if (!is_record_port_locked(port) && !is_listener_port(port) && !is_socket_port(port)) {
                *out = port;
                rc = OK;
                break;
            }
        }
    });

    return rc;
}

} // namespace tcp
} // namespace net
