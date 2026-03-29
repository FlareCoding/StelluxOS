#include "sync/wait_queue.h"
#include "sync/poll.h"
#include "sched/sched.h"
#include "sched/task_exec_core.h"

namespace sync {

/**
 * Set triggered on all observers and wake their tasks.
 *
 * Under wq.lock: set triggered on every observer and snapshot all task
 * pointers into a stack batch. After releasing the lock, wake each task.
 *
 * The observer count per wait_queue is bounded in practice by the number
 * of tasks simultaneously polling the same source (typically 1-2).
 * The batch is sized to 16 which covers all realistic cases. If more
 * observers exist, the excess are handled by a single-entry fallback
 * that re-scans under the lock.
 */
constexpr uint32_t OBSERVER_BATCH_SIZE = 16;

__PRIVILEGED_CODE static void notify_observers_and_unlock(
    wait_queue& wq, irq_state irq
) {
    sched::task* batch[OBSERVER_BATCH_SIZE];
    uint32_t n = 0;
    uint32_t total = 0;

    for (auto& obs : wq.observers) {
        __atomic_store_n(&obs.table->triggered, 1, __ATOMIC_RELEASE);
        if (n < OBSERVER_BATCH_SIZE) {
            batch[n++] = obs.table->task;
        }
        total++;
    }

    spin_unlock_irqrestore(wq.lock, irq);

    for (uint32_t i = 0; i < n; i++) {
        sched::wake(batch[i]);
    }

    // Overflow: re-scan and wake all observers under the lock.
    // Some were already woken above — sched::wake() is idempotent.
    // sched::wake() only acquires rq.lock (never wq.lock), so holding
    // wq.lock here is deadlock-free. This path is extremely rare
    // (requires >16 concurrent pollers on one wait queue).
    if (total > n) {
        irq = spin_lock_irqsave(wq.lock);
        for (auto& obs : wq.observers) {
            sched::wake(obs.table->task);
        }
        spin_unlock_irqrestore(wq.lock, irq);
    }
}

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

    irq_state wq_irq = spin_lock_irqsave(wq.lock);
    if (self->wait_link.is_linked()) {
        wq.waiters.remove(self);
    }
    spin_unlock_irqrestore(wq.lock, wq_irq);

    return spin_lock_irqsave(lock);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_one(wait_queue& wq) {
    irq_state irq = spin_lock_irqsave(wq.lock);
    sched::task* t = wq.waiters.pop_front();

    if (!wq.observers.empty()) {
        // notify_observers_and_unlock releases wq.lock
        notify_observers_and_unlock(wq, irq);
    } else {
        spin_unlock_irqrestore(wq.lock, irq);
    }

    if (t) {
        sched::wake(t);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_all(wait_queue& wq) {
    list::head<sched::task, &sched::task::wait_link> waiter_batch;
    waiter_batch.init();

    irq_state irq = spin_lock_irqsave(wq.lock);
    while (sched::task* t = wq.waiters.pop_front()) {
        waiter_batch.push_back(t);
    }

    if (!wq.observers.empty()) {
        notify_observers_and_unlock(wq, irq);
    } else {
        spin_unlock_irqrestore(wq.lock, irq);
    }

    while (sched::task* t = waiter_batch.pop_front()) {
        sched::wake(t);
    }
}

} // namespace sync
