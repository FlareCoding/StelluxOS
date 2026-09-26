#include "net/tcp/recovery.h"
#include "net/tcp/wire.h"

namespace net {
namespace tcp {

static void enter_state_locked(tcp_conn* conn, recovery_state state) {
    conn->recovery = state;
    if (conn->congestion->set_state) {
        conn->congestion->set_state(conn, state);
    }
}

bool is_duplicate_ack_locked(const tcp_conn* conn, uint8_t flags, size_t payload_len, uint32_t ack,
                             uint32_t window) {
    return conn->snd_nxt != conn->snd_una && payload_len == 0 && !(flags & (FLAG_SYN | FLAG_FIN)) &&
           ack == conn->snd_una && window == conn->snd_wnd;
}

void take_duplicate_ack_locked(tcp_conn* conn) {
    if (conn->dupacks < 0xFF) {
        conn->dupacks++;
    }

    if (conn->recovery == recovery_state::open) {
        enter_state_locked(conn, recovery_state::disorder);
    }
}

void leave_disorder_locked(tcp_conn* conn) {
    conn->dupacks = 0;
    if (conn->recovery == recovery_state::disorder) {
        enter_state_locked(conn, recovery_state::open);
    }
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
