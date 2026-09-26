#ifndef STELLUX_NET_TCP_CONGESTION_H
#define STELLUX_NET_TCP_CONGESTION_H

#include "common/types.h"

namespace net {
namespace tcp {

struct tcp_conn;

constexpr size_t   MAX_CONGESTION_OPS      = 4;
constexpr size_t   CONGESTION_NAME_MAX     = 16;
constexpr size_t   CONGESTION_STATE_SIZE   = 64;
constexpr uint32_t SSTHRESH_INFINITE       = 0x7FFFFFFF; // RFC 5681 3.1
constexpr uint32_t INITIAL_WINDOW_SEGMENTS = 10;         // RFC 6928
constexpr uint32_t INITIAL_WINDOW_CAP      = 14600;      // RFC 6928

/**
 * Where a connection stands in loss recovery (RFC 5681, 6582, 8985). `cwr`
 * is entered only through ECN.
 */
enum class recovery_state : uint8_t {
    open     = 0,
    disorder = 1,
    cwr      = 2,
    recovery = 3,
    loss     = 4,
};

enum class congestion_event : uint8_t {
    tx_start     = 0,
    cwnd_restart = 1,
    complete_cwr = 2,
    loss         = 3,
    ecn_no_ce    = 4,
    ecn_is_ce    = 5,
};

/**
 * One congestion control algorithm, called under the connection lock with
 * `congestion_state` as its own memory. `ssthresh`, `grow`, and `undo` are
 * required, the rest may be null.
 */
struct congestion_ops {
    const char* name;
    void        (*init)(tcp_conn* conn) = nullptr;
    void        (*release)(tcp_conn* conn) = nullptr;
    uint32_t    (*ssthresh)(tcp_conn* conn);
    void        (*grow)(tcp_conn* conn, uint32_t acked_bytes);
    void        (*set_state)(tcp_conn* conn, recovery_state state) = nullptr;
    void        (*event)(tcp_conn* conn, congestion_event which) = nullptr;
    void        (*acked)(tcp_conn* conn, uint32_t bytes, int64_t rtt_us) = nullptr;
    uint32_t    (*undo)(tcp_conn* conn);
};

/**
 * @brief Registers the built-in algorithms and chooses the default.
 */
void init_congestion();

/**
 * @brief Adds an algorithm to the registry under its name.
 * @return OK, ERR_IN_USE when the name is taken, ERR_FULL at capacity.
 */
int32_t register_congestion_ops(const congestion_ops* ops);

/**
 * @brief The algorithm registered under `name`, or nullptr.
 */
const congestion_ops* find_congestion_ops(const char* name);

/**
 * @brief The algorithm a connection gets when its socket chose none.
 */
const congestion_ops* default_congestion_ops();

/**
 * @brief The initial congestion window for `mss` (RFC 6928 2).
 */
uint32_t initial_window(uint16_t mss);

/**
 * @brief True while `cwnd` is below `ssthresh` (RFC 5681 3.1).
 */
bool is_in_slow_start(const tcp_conn* conn);

/**
 * @brief True when `cwnd` is what holds the sender back, the condition for
 * growing it (RFC 5681 3.1, RFC 7661 4.1).
 */
bool is_cwnd_limited(const tcp_conn* conn);

/**
 * @brief Records after a send how much is in flight and whether `cwnd`
 * stopped the burst, per window of data. Caller holds the lock.
 */
void note_cwnd_usage_locked(tcp_conn* conn, bool cwnd_limited);

/**
 * @brief Prepares a send that starts with nothing in flight: after an idle
 * longer than the RTO, `cwnd` restarts from min(cwnd, IW) (RFC 5681 4.1),
 * and the algorithm hears tx_start. Caller holds the lock.
 */
void begin_transmission_locked(tcp_conn* conn, uint64_t now_ns);

/**
 * @brief Grows `cwnd` by the bytes acknowledged, at most two segments (RFC
 * 3465 2.3) and never past `ssthresh`.
 * @return The bytes left over past `ssthresh`.
 */
uint32_t slow_start(tcp_conn* conn, uint32_t acked_bytes);

/**
 * @brief Grows `cwnd` by one segment per `window` bytes acknowledged (RFC
 * 5681 3.1), the remainder carried to the next acknowledgment.
 */
void additive_increase(tcp_conn* conn, uint32_t window, uint32_t acked_bytes);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_CONGESTION_H
