#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "clock/clock.h"
#include "timer/timer.h"
#include "mm/heap.h"

namespace sync {

__PRIVILEGED_CODE void poll_subscribe(poll_table& pt, wait_queue& wq) {
    auto* entry = heap::kalloc_new<poll_entry>();
    if (!entry) {
        pt.error.store_release(1);
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

__PRIVILEGED_CODE bool poll_wait(poll_table& pt, uint64_t timeout_ns) {
    if (pt.triggered.load_acquire()) {
        return true;
    }

    if (pt.error.load_acquire()) {
        return false;
    }

    sched::task* self = pt.task;
    if (signals::interrupt_pending(self)) {
        return false;
    }

    sched::prepare_to_block_task();

    if (sched::block_task_interrupted()) {
        sched::cancel_block_task();
        return false;
    }

    // The interrupt check's fence also orders this re-check against the
    // BLOCKED store, closing the race where a source fires during the transition.
    if (pt.triggered.load_acquire()) {
        sched::cancel_block_task();
        return true;
    }

    if (timeout_ns > 0) {
        uint64_t deadline = clock::now_ns() + timeout_ns;
        timer::schedule_sleep(self, deadline);
    }

    sched::yield();

    timer::cancel_sleep(self);

    return pt.triggered.load_acquire() != 0;
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
