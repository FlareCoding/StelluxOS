#ifndef STELLUX_TIMER_TIMER_H
#define STELLUX_TIMER_TIMER_H

#include "common/types.h"
#include "common/rb_tree.h"
#include "sync/atomic.h"

namespace sched { struct task; }

namespace timer {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

struct deadline_timer;
using deadline_fn = void (*)(deadline_timer* self);

enum class deadline_state : uint8_t {
    idle      = 0,
    scheduled = 1,
};

/**
 * A timer that runs its callback once a deadline passes, embedded in the object
 * that owns it. Callbacks run in a worker task, must not sleep, and may free the timer.
 */
struct deadline_timer {
    rbt::node             link;
    uint64_t              deadline_ns;
    uint64_t              sequence;
    deadline_fn           fn;
    uint32_t              cpu;
    sync::atomic<uint8_t> state;
};

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
 * @brief Scheduler tick frequency configured at init, 0 before init.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t tick_hz();

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

/**
 * @brief Schedules `timer` to run its callback on the calling CPU once
 * `deadline_ns` passes or moves the deadline of a timer already scheduled.
 * A running timer may schedule itself again from its own callback, which is
 * how a periodic timer is written.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void schedule(deadline_timer* timer, uint64_t deadline_ns);

/**
 * @brief Removes `timer` from the tree holding it.
 * @return True when the callback will not run. False when it is already
 *         running and the owner must let it finish. The owner protects
 *         itself against that case by holding a reference for every
 *         scheduled timer and checking a generation counter in the callback.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool cancel(deadline_timer* timer);

// True while `timer` waits in a tree for its deadline
bool is_pending(const deadline_timer* timer);

/**
 * @brief Runs every timer scheduled on this CPU that is due at or before
 * `now_ns` in deadline order on the caller's stack.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void __dbg_test_fire_expired(uint64_t now_ns);

/**
 * @brief Creates one worker task per online CPU to run deadline callbacks.
 * Called once, after every CPU is online and the scheduler runs on each.
 * @return OK, or ERR when a worker could not be created.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t start_deadline_workers();

} // namespace timer

#endif // STELLUX_TIMER_TIMER_H
