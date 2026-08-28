#ifndef STELLUX_SYNC_WAIT_QUEUE_H
#define STELLUX_SYNC_WAIT_QUEUE_H

#include "common/types.h"
#include "common/list.h"
#include "sync/spinlock.h"
#include "sync/poll.h"
#include "sched/task.h"

namespace sync {

struct wait_queue {
    spinlock lock;
    list::head<sched::task, &sched::task::wait_link> waiters;
    list::head<poll_entry, &poll_entry::observer_link> observers;

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

} // namespace sync

#endif // STELLUX_SYNC_WAIT_QUEUE_H
