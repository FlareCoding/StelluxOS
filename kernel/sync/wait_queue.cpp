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
    __atomic_store_n(&self->blocking_kind, sched::TASK_BLOCKING_WAIT_QUEUE, __ATOMIC_RELEASE);
    __atomic_store_n(&self->blocking_object, &wq, __ATOMIC_RELEASE);
    self->state = sched::TASK_STATE_BLOCKED;
    wq.waiters.push_back(self);
    spin_unlock(wq.lock);

    spin_unlock_irqrestore(lock, saved);

    sched::yield();

    irq_state irq = spin_lock_irqsave(lock);
    __atomic_store_n(&self->blocking_kind, sched::TASK_BLOCKING_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&self->blocking_object, nullptr, __ATOMIC_RELEASE);
    return irq;
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

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool cancel_wait(wait_queue& wq, sched::task* t) {
    if (!t) {
        return false;
    }

    irq_state irq = spin_lock_irqsave(wq.lock);
    bool matches_queue =
        __atomic_load_n(&t->blocking_kind, __ATOMIC_ACQUIRE) == sched::TASK_BLOCKING_WAIT_QUEUE &&
        __atomic_load_n(&t->blocking_object, __ATOMIC_ACQUIRE) == &wq;
    bool linked = t->wait_link.prev != nullptr && t->wait_link.next != nullptr;

    if (matches_queue && linked) {
        wq.waiters.remove(t);
        __atomic_store_n(&t->blocking_kind, sched::TASK_BLOCKING_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&t->blocking_object, nullptr, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(wq.lock, irq);
        return true;
    }

    spin_unlock_irqrestore(wq.lock, irq);
    return false;
}

} // namespace sync
