#include "signals/signal.h"
#include "sched/task.h"

namespace signals {

/**
 * Clear pending instances of sig from the shared set and every thread.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void discard_pending(sched::thread_group* tg, uint32_t sig) {
    const sig_set_t keep = ~sig_bit(sig);
    __atomic_fetch_and(&tg->sig.shared_pending, keep, __ATOMIC_ACQ_REL);

    sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);
    if (tg->leader) {
        __atomic_fetch_and(&tg->leader->sig.pending, keep, __ATOMIC_ACQ_REL);
    }
    for (sched::task& thread : tg->threads) {
        __atomic_fetch_and(&thread.sig.pending, keep, __ATOMIC_ACQ_REL);
    }
    sync::spin_unlock_irqrestore(tg->lock, irq);
}

__PRIVILEGED_CODE int32_t set_action(sched::thread_group* tg, uint32_t sig,
                                     const k_sigaction* act, k_sigaction* old) {
    if (!tg || !sig_valid(sig)) {
        return ERR_INVAL;
    }
    if (act && (sig == SIGKILL || sig == SIGSTOP)) {
        return ERR_INVAL;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(tg->sig.lock);
    if (old) {
        *old = tg->sig.actions[sig - 1];
    }
    if (act) {
        tg->sig.actions[sig - 1] = *act;
        // Discard while holding sig.lock so a concurrent sigaction cannot
        // install a handler between the install and the flush. Nests
        // tg->lock inside sig.lock, no path takes them in reverse order.
        if (act->handler == SIG_IGN) {
            discard_pending(tg, sig);
        }
    }
    sync::spin_unlock_irqrestore(tg->sig.lock, irq);
    return OK;
}

__PRIVILEGED_CODE int32_t set_blocked(sched::task* t, uint32_t how,
                                      const sig_set_t* set, sig_set_t* old) {
    if (!t) {
        return ERR_INVAL;
    }

    sig_set_t cur = __atomic_load_n(&t->sig.blocked, __ATOMIC_ACQUIRE);
    if (old) {
        *old = cur;
    }
    if (!set) {
        return OK;
    }

    sig_set_t next;
    switch (how) {
        case SIG_BLOCK:   next = cur | *set;  break;
        case SIG_UNBLOCK: next = cur & ~*set; break;
        case SIG_SETMASK: next = *set;        break;
        default:          return ERR_INVAL;
    }
    next &= ~UNBLOCKABLE_MASK;
    __atomic_store_n(&t->sig.blocked, next, __ATOMIC_RELEASE);
    return OK;
}

__PRIVILEGED_CODE sig_set_t pending_blocked_set(sched::task* t) {
    sig_set_t pend = __atomic_load_n(&t->sig.pending, __ATOMIC_ACQUIRE);
    if (t->group) {
        pend |= __atomic_load_n(&t->group->sig.shared_pending, __ATOMIC_ACQUIRE);
    }
    return pend & __atomic_load_n(&t->sig.blocked, __ATOMIC_ACQUIRE);
}

} // namespace signals
