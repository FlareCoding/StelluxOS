#ifndef STELLUX_SYNC_WAIT_QUEUE_H
#define STELLUX_SYNC_WAIT_QUEUE_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"
#include "sched/task.h"

namespace sync {

struct wait_queue {
    spinlock lock;
    list::head<sched::task, &sched::task::wait_link> waiters;

    void init() {
        lock = SPINLOCK_INIT;
        waiters.init();
    }
};

/**
 * Block current task until woken, atomically releasing a held lock.
 *
 * Caller MUST hold `lock` via spin_lock_irqsave (IRQs disabled).
 * Internally acquires wq.lock using plain spin_lock (safe: IRQs
 * already disabled), registers the task, then releases both locks
 * before yielding.
 *
 * On wake, re-acquires `lock` via spin_lock_irqsave and returns
 * the new irq_state. Caller MUST re-check its condition (spurious
 * wakeups are permitted).
 *
 * Usage:
 *   irq_state irq = spin_lock_irqsave(lock);
 *   while (!condition) {
 *       irq = sync::wait(wq, lock, irq);
 *   }
 *   spin_unlock_irqrestore(lock, irq);
 *
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE
irq_state wait(wait_queue& wq, spinlock& lock, irq_state saved);

/**
 * Wake the first waiting task (FIFO order).
 * No-op if the queue is empty. Safe from IRQ context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_one(wait_queue& wq);

/**
 * Wake all waiting tasks.
 * No-op if the queue is empty. Safe from IRQ context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_all(wait_queue& wq);

/**
 * Remove a specific blocked task from the wait queue if present.
 * Safe from IRQ context.
 * @return true if the task was removed from this wait queue.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE bool cancel_wait(wait_queue& wq, sched::task* t);

} // namespace sync

#endif // STELLUX_SYNC_WAIT_QUEUE_H
