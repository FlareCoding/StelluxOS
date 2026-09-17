#ifndef STELLUX_NET_TCP_SEQ_H
#define STELLUX_NET_TCP_SEQ_H

#include "common/types.h"

namespace net {
namespace tcp {

/**
 * Sequence number comparisons modulo 2^32 (RFC 9293 3.4). Two numbers order
 * by the sign of their difference, so a window that wraps past zero compares
 * like any other as long as the numbers lie less than 2^31 apart.
 */
inline bool seq_lt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
inline bool seq_leq(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }
inline bool seq_gt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
inline bool seq_geq(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }

/**
 * @brief True when `seq` lies within `low` to `high`, both ends included, for
 * a range that may wrap past zero.
 */
inline bool seq_between(uint32_t seq, uint32_t low, uint32_t high) {
    return high - low >= seq - low;
}

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_SEQ_H
