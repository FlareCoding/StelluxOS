#ifndef STELLUX_NET_TCP_TIMERS_H
#define STELLUX_NET_TCP_TIMERS_H

#include "net/tcp/record.h"
#include "timer/timer.h"

namespace net {
namespace tcp {

/**
 * @brief Schedules `timer` for `deadline_ns`, holding a reference on `rec` for
 * the timer until its callback finishes or a disarm removes it. The caller
 * records the deadline in `rec` so the callback can tell a live arming from a
 * stale one.
 */
void arm_timer(record* rec, timer::deadline_timer* timer, uint64_t deadline_ns);

/**
 * @brief Stops `timer` and drops the reference it held. A callback already
 * running keeps its reference and drops it itself when it finishes.
 */
void disarm_timer(record* rec, timer::deadline_timer* timer);

/**
 * @brief Drops the reference a finished callback held for `rec`.
 */
void finish_timer_callback(record* rec);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_TIMERS_H
