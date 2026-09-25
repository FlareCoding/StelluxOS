#ifndef STELLUX_SYNC_WAIT_QUEUE_H
#define STELLUX_SYNC_WAIT_QUEUE_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"
#include "sched/task.h"

namespace sync {

struct wait_observer;

// Runs under the queue's lock on every wake and returns a task to wake once the lock drops, or null.
// It must not block or wake tasks itself, and must return a task only when it newly owes that task a wake.
using observer_notify_fn = sched::task* (*)(wait_observer& observer);

// Reacts to a wait queue's wakes without sleeping on the queue
struct wait_observer {
    list::node         link;
    observer_notify_fn notify;
};

struct wait_queue {
    spinlock lock;
    list::head<sched::task, &sched::task::wait_link> waiters;
    list::head<wait_observer, &wait_observer::link> observers;

    void init() {
        lock = SPINLOCK_INIT;
        waiters.init();
        observers.init();
    }
};

/**
 * Block current task until woken, atomically releasing a held lock.
 *
 * Caller MUST hold `lock` via spin_lock_irqsave (IRQs disabled).
 * Takes wq.lock for the enqueue and the post-yield cleanup, so an ISR
 * running wake_one() or wake_all() cannot race the wait entry.
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
 * No-op if the queue is empty. Waiters are pinned internally, but the
 * off-CPU spin rule of sched::wake still applies to the caller.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_one(wait_queue& wq);

/**
 * Wake all waiting tasks.
 * No-op if the queue is empty. Waiters are pinned internally, but the
 * off-CPU spin rule of sched::wake still applies to the caller.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_all(wait_queue& wq);

/**
 * Attach an observer, so later wakes on wq run its notify callback.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void add_observer(wait_queue& wq, wait_observer& observer);

/**
 * Detach an observer. Once this returns, no wake on wq runs its callback.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void remove_observer(wait_queue& wq, wait_observer& observer);

} // namespace sync

#endif // STELLUX_SYNC_WAIT_QUEUE_H
