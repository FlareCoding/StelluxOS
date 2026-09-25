#include "sync/wait_queue.h"
#include "sched/sched.h"
#include "sched/task_exec_core.h"

namespace sync {

constexpr uint32_t OBSERVER_BATCH_SIZE = 16;
constexpr uint32_t WAITER_BATCH_SIZE   = 16;

/**
 * Runs each observer's callback and wakes the returned tasks after dropping wq.lock. A full
 * batch rescans, which is safe since callbacks return a task only when they newly owe a wake.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void notify_observers_and_unlock(
    wait_queue& wq, irq_state irq
) {
    for (;;) {
        rc::strong_ref<sched::task> batch[OBSERVER_BATCH_SIZE];
        uint32_t n = 0;
        bool rescan = false;

        for (auto& obs : wq.observers) {
            if (n == OBSERVER_BATCH_SIZE) {
                rescan = true;
                break;
            }

            sched::task* owed = obs.notify(obs);
            if (owed) {
                batch[n++] = sched::task_ref(owed);
            }
        }

        spin_unlock_irqrestore(wq.lock, irq);

        for (uint32_t i = 0; i < n; i++) {
            if (batch[i]) {
                sched::wake(batch[i].ptr());
            }
        }

        if (!rescan) {
            return;
        }

        irq = spin_lock_irqsave(wq.lock);
    }
}

/**
 * Not interruptible on its own: a force-woken waiter re-blocks unless
 * the caller loops on signals::interrupt_pending().
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
    sched::prepare_to_block_task();
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
    rc::strong_ref<sched::task> t = sched::task_ref(wq.waiters.pop_front());

    if (!wq.observers.empty()) {
        // notify_observers_and_unlock releases wq.lock
        notify_observers_and_unlock(wq, irq);
    } else {
        spin_unlock_irqrestore(wq.lock, irq);
    }

    if (t) {
        sched::wake(t.ptr());
    }
}

/**
 * Wake all waiting tasks. Snapshots counted waiter references into a stack
 * batch so wait_link is fully unlinked (prev=next=nullptr) before any task
 * can be scheduled. This prevents a concurrent force_wake_for_kill from
 * racing with post-yield cleanup in sync::wait, which assumes is_linked
 * means "still on wq.waiters".
 *
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void wake_all(wait_queue& wq) {
    for (;;) {
        rc::strong_ref<sched::task> batch[WAITER_BATCH_SIZE];
        uint32_t n = 0;
        irq_state irq = spin_lock_irqsave(wq.lock);

        while (!wq.waiters.empty() && n < WAITER_BATCH_SIZE) {
            batch[n++] = sched::task_ref(wq.waiters.pop_front());
        }
        bool drained = wq.waiters.empty();

        if (drained && !wq.observers.empty()) {
            notify_observers_and_unlock(wq, irq);
        } else {
            spin_unlock_irqrestore(wq.lock, irq);
        }

        for (uint32_t i = 0; i < n; i++) {
            if (batch[i]) {
                sched::wake(batch[i].ptr());
            }
        }

        if (drained) break;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void add_observer(wait_queue& wq, wait_observer& observer) {
    irq_state irq = spin_lock_irqsave(wq.lock);
    wq.observers.push_back(&observer);
    spin_unlock_irqrestore(wq.lock, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void remove_observer(wait_queue& wq, wait_observer& observer) {
    irq_state irq = spin_lock_irqsave(wq.lock);
    if (observer.link.is_linked()) {
        wq.observers.remove(&observer);
    }
    spin_unlock_irqrestore(wq.lock, irq);
}

} // namespace sync
