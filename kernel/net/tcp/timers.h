#ifndef STELLUX_NET_TCP_TIMERS_H
#define STELLUX_NET_TCP_TIMERS_H

#include "net/tcp/conn.h"
#include "timer/timer.h"

namespace net {
namespace tcp {

/**
 * @brief Arms the connection's send timer as `kind`, its deadline the initial
 * timeout doubled once per retransmission and capped. Caller holds the lock.
 */
void arm_send_timer_locked(tcp_conn* conn, timer_kind kind);

/**
 * @brief Arms the send timer as the orphan's wait in FIN_WAIT_2 for the peer's
 * FIN, after which the connection is reset. Caller holds the lock.
 */
void arm_orphan_timer_locked(tcp_conn* conn);

/**
 * @brief The send timer's callback: retransmits what the connection's state
 * still waits for, or gives the connection up once the retries are spent.
 */
void on_send_timer(timer::deadline_timer* timer);

/**
 * @brief Marks an ACK as owed and arms the ack timer for it unless one is
 * already waiting. Caller holds the lock.
 */
void delay_ack_locked(tcp_conn* conn);

/**
 * @brief The ack timer's callback: sends the owed ACK once the delay is over.
 */
void on_ack_timer(timer::deadline_timer* timer);

/**
 * @brief Schedules `timer` for `deadline_ns`, holding one reference on `rec`
 * for as long as the timer is scheduled, however often it is moved. The caller
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
