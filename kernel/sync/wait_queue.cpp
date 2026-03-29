#include "sync/wait_queue.h"
#include "sync/poll.h"
#include "sched/sched.h"
#include "sched/task_exec_core.h"

namespace sync {

// Max observer tasks to batch on stack per wake call.
// Bounded to avoid unbounded stack usage in IRQ context.
constexpr uint32_t MAX_OBSERVER_BATCH = 8;

/**
 * Collect observer tasks to wake. Called under wq.lock.
 * Sets triggered flag on each observer's poll_table and snapshots
 * the task pointer for deferred wake after lock release.
 */
__PRIVILEGED_CODE static uint32_t collect_observer_tasks(
    wait_queue& wq, sched::task** out, uint32_t capacity
) {
    uint32_t count = 0;
    for (auto& obs : wq.observers) {
        __atomic_store_n(&obs.table->triggered, 1, __ATOMIC_RELEASE);
        if (count < capacity) {
            out[count++] = obs.table->task;
        }
    }
    return count;
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
    sched::task* observer_tasks[MAX_OBSERVER_BATCH];
    uint32_t observer_count = 0;

    irq_state irq = spin_lock_irqsave(wq.lock);
    sched::task* t = wq.waiters.pop_front();
    if (!wq.observers.empty()) {
        observer_count = collect_observer_tasks(wq, observer_tasks, MAX_OBSERVER_BATCH);
    }
    spin_unlock_irqrestore(wq.lock, irq);

    if (t) {
        sched::wake(t);
    }
    for (uint32_t i = 0; i < observer_count; i++) {
        sched::wake(observer_tasks[i]);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_all(wait_queue& wq) {
    list::head<sched::task, &sched::task::wait_link> batch;
    batch.init();

    sched::task* observer_tasks[MAX_OBSERVER_BATCH];
    uint32_t observer_count = 0;

    irq_state irq = spin_lock_irqsave(wq.lock);
    while (sched::task* t = wq.waiters.pop_front()) {
        batch.push_back(t);
    }
    if (!wq.observers.empty()) {
        observer_count = collect_observer_tasks(wq, observer_tasks, MAX_OBSERVER_BATCH);
    }
    spin_unlock_irqrestore(wq.lock, irq);

    while (sched::task* t = batch.pop_front()) {
        sched::wake(t);
    }
    for (uint32_t i = 0; i < observer_count; i++) {
        sched::wake(observer_tasks[i]);
    }
}

} // namespace sync
