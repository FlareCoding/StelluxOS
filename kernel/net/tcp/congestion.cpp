#include "net/tcp/congestion.h"
#include "net/tcp/conn.h"
#include "net/tcp/seq.h"
#include "net/net.h"
#include "common/string.h"

namespace net {
namespace tcp {

static const congestion_ops* g_registry[MAX_CONGESTION_OPS];
static size_t                g_registered;
static const congestion_ops* g_default;

static uint32_t newreno_ssthresh(tcp_conn* conn) {
    uint32_t half = conn->cwnd / 2;
    uint32_t floor = 2u * conn->snd_mss;
    return half > floor ? half : floor;
}

static void newreno_grow(tcp_conn* conn, uint32_t acked_bytes) {
    if (is_in_slow_start(conn)) {
        acked_bytes = slow_start(conn, acked_bytes);
        if (acked_bytes == 0) {
            return;
        }
    }

    additive_increase(conn, conn->cwnd, acked_bytes);
}

static uint32_t newreno_undo(tcp_conn* conn) {
    return conn->cwnd > conn->prior_cwnd ? conn->cwnd : conn->prior_cwnd;
}

static const congestion_ops NEWRENO = {
    .name     = "newreno",
    .ssthresh = newreno_ssthresh,
    .grow     = newreno_grow,
    .undo     = newreno_undo,
};

void init_congestion() {
    (void)register_congestion_ops(&NEWRENO);
    g_default = &NEWRENO;
}

int32_t register_congestion_ops(const congestion_ops* ops) {
    if (find_congestion_ops(ops->name)) {
        return ERR_IN_USE;
    }

    if (g_registered == MAX_CONGESTION_OPS) {
        return ERR_FULL;
    }

    g_registry[g_registered++] = ops;
    return OK;
}

const congestion_ops* find_congestion_ops(const char* name) {
    for (size_t i = 0; i < g_registered; i++) {
        if (string::strncmp(g_registry[i]->name, name, CONGESTION_NAME_MAX) == 0) {
            return g_registry[i];
        }
    }

    return nullptr;
}

const congestion_ops* default_congestion_ops() {
    return g_default;
}

uint32_t initial_window(uint16_t mss) {
    uint32_t floor = 2u * mss > INITIAL_WINDOW_CAP ? 2u * mss : INITIAL_WINDOW_CAP;
    uint32_t window = INITIAL_WINDOW_SEGMENTS * mss;
    return window < floor ? window : floor;
}

bool is_in_slow_start(const tcp_conn* conn) {
    return conn->cwnd < conn->ssthresh;
}

bool is_cwnd_limited(const tcp_conn* conn) {
    if (is_in_slow_start(conn)) {
        return conn->cwnd < 2 * conn->max_in_flight;
    }

    return conn->cwnd_limited;
}

void note_cwnd_usage_locked(tcp_conn* conn, bool cwnd_limited) {
    uint32_t in_flight = conn->snd_nxt - conn->snd_una;
    bool window_over = !seq_lt(conn->snd_una, conn->in_flight_window_end);
    if (window_over || in_flight > conn->max_in_flight || cwnd_limited) {
        conn->max_in_flight = in_flight;
        conn->in_flight_window_end = conn->snd_nxt;
        conn->cwnd_limited = cwnd_limited;
    }
}

uint32_t slow_start(tcp_conn* conn, uint32_t acked_bytes) {
    uint32_t step = acked_bytes < 2u * conn->snd_mss ? acked_bytes : 2u * conn->snd_mss;
    uint32_t room = conn->ssthresh - conn->cwnd;
    uint32_t used = step < room ? step : room;
    conn->cwnd += used;

    return step - used;
}

void additive_increase(tcp_conn* conn, uint32_t window, uint32_t acked_bytes) {
    conn->bytes_acked += acked_bytes;
    if (conn->bytes_acked >= window) {
        uint32_t segments = conn->bytes_acked / window;
        conn->bytes_acked -= segments * window;
        conn->cwnd += segments * conn->snd_mss;
    }
}

} // namespace tcp
} // namespace net
