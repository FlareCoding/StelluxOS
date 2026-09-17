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

// Only `removed` means a scheduled timer was stopped, so only
// that caller owns whatever the callback would have released.
enum class cancel_outcome : uint8_t {
    not_scheduled = 0,
    removed       = 1,
    running       = 2,
};

/**
 * A timer that runs its callback once a deadline passes, embedded in the object
 * that owns it. Callbacks run lowered in a worker task and must not sleep.
 */
struct deadline_timer {
    rbt::node              link;
    uint64_t               deadline_ns;
    uint64_t               sequence;
    deadline_fn            fn;
    sync::atomic<uint32_t> cpu;
    sync::atomic<uint8_t>  state;
};

// Puts an unscheduled `timer` in the idle state with `fn` as its callback
inline void init_deadline_timer(deadline_timer* timer, deadline_fn fn) {
    timer->link = {};
    timer->deadline_ns = 0;
    timer->sequence = 0;
    timer->fn = fn;
    timer->cpu.store_relaxed(0);
    timer->state.store_relaxed(static_cast<uint8_t>(deadline_state::idle));
}

// The object holding `timer` as its member `Member`
template <typename T, deadline_timer T::*Member>
inline T* owner_of(deadline_timer* timer) {
    const uintptr_t offset = reinterpret_cast<uintptr_t>(&(static_cast<T*>(nullptr)->*Member));
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(timer) - offset);
}

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
 * @brief Wakes `task` once `deadline_ns` passes, through the deadline timer
 * embedded in it. The task must already be TASK_STATE_BLOCKED. The scheduled
 * timer holds a reference to the task until it fires or is cancelled.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void schedule_sleep(sched::task* task, uint64_t deadline_ns);

/**
 * @brief Stops the sleep timer of `task` if it has not fired. Never waits, so
 * it is safe from any context, interrupt handlers included.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cancel_sleep(sched::task* task);

/**
 * @brief Schedules `timer` to run its callback on the calling CPU once
 * `deadline_ns` passes or moves the deadline of a timer already scheduled.
 * Safe against a concurrent `schedule` or `cancel` of the same timer from any
 * CPU. A running timer may schedule itself again from its own callback,
 * elevated like every privileged call made lowered, which is how a periodic
 * timer is written.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void schedule(deadline_timer* timer, uint64_t deadline_ns);

/**
 * @brief Removes `timer` from the tree holding it. Never waits.
 * @return `removed` when a scheduled timer was stopped, `not_scheduled` when
 *         there was nothing to stop, `running` when the callback has already
 *         started and will finish on its own. An owner that must outlive a
 *         running callback holds a reference for every scheduled timer.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE cancel_outcome cancel(deadline_timer* timer);

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
