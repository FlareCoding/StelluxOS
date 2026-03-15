#ifndef STELLUX_TIMER_TIMER_H
#define STELLUX_TIMER_TIMER_H

#include "common/types.h"

namespace sched { struct task; }

namespace timer {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * @brief Initialize the timer subsystem on the BSP.
 * Calibrates hardware timer, programs first one-shot tick, enables IRQs.
 * Must be called after irq::init(), sched::init(), and clock::init().
 * @param hz Scheduler tick frequency (e.g. 100 for 10ms quantum).
 * @return OK on success, ERR on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init(uint32_t hz);

/**
 * @brief Initialize the timer subsystem on an AP.
 * Reuses BSP calibration, programs first one-shot tick, enables IRQs.
 * Must be called after irq::init_ap() and sched::init_ap().
 * @param hz Scheduler tick frequency (same as BSP).
 * @return OK on success, ERR on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap(uint32_t hz);

/**
 * @brief Stop the timer and mask its interrupt.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void stop();

/**
 * @brief Timer interrupt handler. Wakes expired sleepers, advances the
 * scheduler tick, and reprograms the hardware for the next event.
 * Called from the arch trap handler on timer interrupt.
 * @return true if a scheduler tick expired (caller should call sched::on_tick).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool on_interrupt();

/**
 * @brief Schedule a task to be woken at the given absolute deadline.
 * Inserts the task into the per-CPU sleep queue and reprograms the
 * hardware timer if the new deadline is sooner than the current one.
 * The task's state must already be TASK_STATE_BLOCKED before this call.
 * @param t Task to sleep (must be the current task on this CPU).
 * @param deadline_ns Absolute wakeup time in nanoseconds (clock::now_ns() timebase).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void schedule_sleep(sched::task* t, uint64_t deadline_ns);

/**
 * @brief Remove a task from its CPU's sleep queue if present.
 * No-op if the task is not on any sleep queue. Safe to call from
 * any CPU. Must be called from elevated/privileged context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cancel_sleep(sched::task* t);

} // namespace timer

#endif // STELLUX_TIMER_TIMER_H
