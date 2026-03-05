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

    int terminate_exit = 0;
    if (sched::termination_requested(self, &terminate_exit)) {
        spin_unlock_irqrestore(lock, saved);
        sched::exit(terminate_exit);
    }

    spin_lock(wq.lock);
    self->state = sched::TASK_STATE_BLOCKED;
    self->block_kind = sched::TASK_BLOCK_WAIT_QUEUE;
    self->blocked_wait_queue = &wq;
    wq.waiters.push_back(self);
    spin_unlock(wq.lock);

    spin_unlock_irqrestore(lock, saved);

    sched::yield();
    sched::maybe_terminate_current();

    return spin_lock_irqsave(lock);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_one(wait_queue& wq) {
    irq_state irq = spin_lock_irqsave(wq.lock);
    sched::task* t = wq.waiters.pop_front();
    if (t) {
        t->block_kind = sched::TASK_BLOCK_NONE;
        t->blocked_wait_queue = nullptr;
    }
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
        t->block_kind = sched::TASK_BLOCK_NONE;
        t->blocked_wait_queue = nullptr;
        batch.push_back(t);
    }
    spin_unlock_irqrestore(wq.lock, irq);

    while (sched::task* t = batch.pop_front()) {
        sched::wake(t);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void cancel_wait(wait_queue& wq, sched::task* task) {
    if (!task) {
        return;
    }

    bool should_wake = false;
    irq_state irq = spin_lock_irqsave(wq.lock);
    if (task->state == sched::TASK_STATE_BLOCKED &&
        task->block_kind == sched::TASK_BLOCK_WAIT_QUEUE &&
        task->blocked_wait_queue == &wq &&
        task->wait_link.prev && task->wait_link.next) {
        wq.waiters.remove(task);
        task->block_kind = sched::TASK_BLOCK_NONE;
        task->blocked_wait_queue = nullptr;
        should_wake = true;
    }
    spin_unlock_irqrestore(wq.lock, irq);

    if (should_wake) {
        sched::wake(task);
    }
}

} // namespace sync
