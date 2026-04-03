#include "sync/mutex.h"
#include "sched/sched.h"
#include "sched/task_exec_core.h"
#include "common/logging.h"

#ifdef DEBUG
#define MUTEX_ASSERT(cond, msg) \
    do { if (!(cond)) log::fatal("mutex: " msg); } while(0)
#else
#define MUTEX_ASSERT(cond, msg) ((void)0)
#endif

namespace sync {

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mutex_lock(mutex& m) {
    sched::task* self = sched::current();

    irq_state irq = spin_lock_irqsave(m.lock);

    MUTEX_ASSERT(m.owner != self, "recursive lock detected");

    while (m.owner != nullptr) {
        MUTEX_ASSERT(!(self->exec.flags & sched::TASK_FLAG_IDLE),
                     "idle task blocked on contended mutex");
        irq = wait(m.wq, m.lock, irq);
    }

    m.owner = self;
    spin_unlock_irqrestore(m.lock, irq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mutex_unlock(mutex& m) {
    irq_state irq = spin_lock_irqsave(m.lock);

    MUTEX_ASSERT(m.owner == sched::current(),
                 "unlock called by non-owner");

    m.owner = nullptr;
    spin_unlock_irqrestore(m.lock, irq);

    wake_one(m.wq);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool mutex_trylock(mutex& m) {
    irq_state irq = spin_lock_irqsave(m.lock);

    if (m.owner != nullptr) {
        spin_unlock_irqrestore(m.lock, irq);
        return false;
    }

    m.owner = sched::current();
    spin_unlock_irqrestore(m.lock, irq);
    return true;
}

} // namespace sync

