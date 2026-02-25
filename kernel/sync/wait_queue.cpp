#include "sync/wait_queue.h"
#include "sched/sched.h"
#include "sched/task_exec_core.h"

namespace sync {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
irq_state wait(wait_queue& wq, spinlock& lock, irq_state saved) {
    sched::task* self = sched::current();

    if (self->exec.flags & sched::TASK_FLAG_IDLE) {
        for (;;) {
            cpu::halt();
        }
    }

    spin_lock(wq.lock);
    self->state = sched::TASK_STATE_BLOCKED;
    wq.waiters.push_back(self);
    spin_unlock(wq.lock);

    spin_unlock_irqrestore(lock, saved);

    sched::yield();

    return spin_lock_irqsave(lock);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_one(wait_queue& wq) {
    irq_state irq = spin_lock_irqsave(wq.lock);
    sched::task* t = wq.waiters.pop_front();
    spin_unlock_irqrestore(wq.lock, irq);

    if (t) {
        sched::wake(t);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_all(wait_queue& wq) {
    list::head<sched::task, &sched::task::wait_link> batch;
    batch.init();

    irq_state irq = spin_lock_irqsave(wq.lock);
    while (sched::task* t = wq.waiters.pop_front()) {
        batch.push_back(t);
    }
    spin_unlock_irqrestore(wq.lock, irq);

    while (sched::task* t = batch.pop_front()) {
        sched::wake(t);
    }
}

} // namespace sync
