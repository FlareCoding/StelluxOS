#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "mm/heap.h"

namespace sync {

__PRIVILEGED_CODE void poll_subscribe(poll_table& pt, wait_queue& wq) {
    auto* entry = heap::kalloc_new<poll_entry>();
    if (!entry) {
        __atomic_store_n(&pt.error, 1, __ATOMIC_RELEASE);
        return;
    }

    entry->table = &pt;
    entry->source = &wq;

    irq_state irq = spin_lock_irqsave(pt.lock);
    pt.entries.push_back(entry);
    spin_unlock_irqrestore(pt.lock, irq);

    irq = spin_lock_irqsave(wq.lock);
    wq.observers.push_back(entry);
    spin_unlock_irqrestore(wq.lock, irq);
}

__PRIVILEGED_CODE static void resolve_aborted_poll_block(sched::task* self) {
    uint32_t expected = sched::TASK_STATE_BLOCKED;
    if (__atomic_compare_exchange_n(&self->state, &expected,
            sched::TASK_STATE_RUNNING, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }

    // A concurrent wake already moved us to READY and queued a runqueue entry.
    // Yield once so the scheduler can consume that entry instead of leaving
    // sched_link live while we keep executing in C.
    sched::yield();
}

__PRIVILEGED_CODE bool poll_wait(poll_table& pt, uint64_t timeout_ns) {
    if (__atomic_load_n(&pt.triggered, __ATOMIC_ACQUIRE)) {
        return true;
    }

    if (__atomic_load_n(&pt.error, __ATOMIC_ACQUIRE)) {
        return false;
    }

    sched::task* self = pt.task;
    if (__atomic_load_n(&self->kill_pending, __ATOMIC_ACQUIRE)) {
        return false;
    }

    self->state = sched::TASK_STATE_BLOCKED;

    // Re-check after setting BLOCKED to close races where a source fires
    // or kill arrives between the initial checks and the state transition.
    if (__atomic_load_n(&pt.triggered, __ATOMIC_ACQUIRE)) {
        resolve_aborted_poll_block(self);
        return true;
    }
    if (__atomic_load_n(&self->kill_pending, __ATOMIC_ACQUIRE)) {
        resolve_aborted_poll_block(self);
        return false;
    }

    if (timeout_ns > 0) {
        uint64_t deadline = clock::now_ns() + timeout_ns;
        timer::schedule_sleep(self, deadline);
    }

    sched::yield();

    timer::cancel_sleep(self);
    return __atomic_load_n(&pt.triggered, __ATOMIC_ACQUIRE) != 0;
}

__PRIVILEGED_CODE void poll_cleanup(poll_table& pt) {
    irq_state pt_irq = spin_lock_irqsave(pt.lock);
    while (poll_entry* entry = pt.entries.pop_front()) {
        irq_state wq_irq = spin_lock_irqsave(entry->source->lock);
        if (entry->observer_link.is_linked()) {
            entry->source->observers.remove(entry);
        }
        spin_unlock_irqrestore(entry->source->lock, wq_irq);
        heap::kfree_delete(entry);
    }
    spin_unlock_irqrestore(pt.lock, pt_irq);
}

} // namespace sync
