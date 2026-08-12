#include "signals/signal.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "timer/timer.h"
#include "common/logging.h"

namespace signals {

// What a send must do for sig, given the process's installed action
enum class send_verdict : uint8_t {
    FATAL,     // default-terminate, wake the target so it can die
    IGNORABLE, // droppable unless the target blocks it
    HANDLED,   // a user handler is installed, leave it pending
};

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
        k_sigaction stored = *act;

        // POSIX: the handler mask can never block SIGKILL/SIGSTOP
        stored.mask &= ~UNBLOCKABLE_MASK;
        tg->sig.actions[sig - 1] = stored;

        // POSIX: an ignoring disposition discards pending instances. Doing
        // it under sig.lock keeps install and flush atomic (nests tg->lock).
        bool ignores = stored.handler == SIG_IGN ||
            (stored.handler == SIG_DFL && dfl_action(sig) == default_action::IGNORE);

        if (ignores) {
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

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static send_verdict classify_send(sched::thread_group* tg,
                                                    uint32_t sig) {
    sync::irq_state irq = sync::spin_lock_irqsave(tg->sig.lock);
    uintptr_t handler = tg->sig.actions[sig - 1].handler;
    sync::spin_unlock_irqrestore(tg->sig.lock, irq);

    if (handler != SIG_DFL && handler != SIG_IGN) {
        return send_verdict::HANDLED;
    }
    if (handler == SIG_IGN) {
        return send_verdict::IGNORABLE;
    }

    switch (dfl_action(sig)) {
        case default_action::TERM:
            return send_verdict::FATAL;
        case default_action::STOP:
            log::warn("signals: stop/continue unsupported, ignoring signal %u", sig);
            return send_verdict::IGNORABLE;
        case default_action::IGNORE:
        default:
            return send_verdict::IGNORABLE;
    }
}

/**
 * Wake a blocked task so it observes a newly pending signal. The fence
 * pairs with the interruption re-check after prepare_to_block_task.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wake_for_signal(sched::task* t) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    timer::cancel_sleep(t);
    sched::wake(t);
}

__PRIVILEGED_CODE int32_t send_to_task(sched::task* t, uint32_t sig) {
    if (!t || !sig_valid(sig)) {
        return ERR_INVAL;
    }

    if ((t->exec.flags & (sched::TASK_FLAG_KERNEL | sched::TASK_FLAG_IDLE)) || !t->group) {
        return ERR_PERM;
    }

    if (sig == SIGKILL) {
        // SIGKILL is process-wide: the shared bit is fatal for every thread
        // at its next kernel crossing, not only after leader teardown
        sched::thread_group* tg = t->group;
        __atomic_fetch_or(&tg->sig.shared_pending, sig_bit(SIGKILL), __ATOMIC_ACQ_REL);
        sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);

        if (tg->leader && tg->leader != t) {
            sched::force_wake_for_kill(tg->leader);
        }

        sync::spin_unlock_irqrestore(tg->lock, irq);
        sched::force_wake_for_kill(t);
        return OK;
    }

    send_verdict verdict = classify_send(t->group, sig);
    sig_set_t blocked = __atomic_load_n(&t->sig.blocked, __ATOMIC_ACQUIRE);
    bool is_blocked = (blocked & sig_bit(sig)) != 0;

    // Ignored signals are dropped unless blocked (the action may change
    // before the target unblocks them)
    if (verdict == send_verdict::IGNORABLE && !is_blocked) {
        return OK;
    }

    __atomic_fetch_or(&t->sig.pending, sig_bit(sig), __ATOMIC_ACQ_REL);
    if (verdict == send_verdict::FATAL && !is_blocked) {
        wake_for_signal(t);
    }
    return OK;
}

__PRIVILEGED_CODE int32_t send_to_group(sched::thread_group* tg, uint32_t sig) {
    if (!tg || !sig_valid(sig)) {
        return ERR_INVAL;
    }

    if (sig == SIGKILL) {
        __atomic_fetch_or(&tg->sig.shared_pending, sig_bit(SIGKILL), __ATOMIC_ACQ_REL);
        sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);

        if (tg->leader) {
            sched::force_wake_for_kill(tg->leader); // leader exit reaps the group
        }

        sync::spin_unlock_irqrestore(tg->lock, irq);
        return OK;
    }

    send_verdict verdict = classify_send(tg, sig);
    const sig_set_t bit = sig_bit(sig);

    sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);

    if (verdict == send_verdict::IGNORABLE) {
        // Droppable only if no thread has it blocked
        bool blocked_somewhere = false;
        if (tg->leader &&
            (__atomic_load_n(&tg->leader->sig.blocked, __ATOMIC_ACQUIRE) & bit)) {
            blocked_somewhere = true;
        }

        for (sched::task& thread : tg->threads) {
            if (__atomic_load_n(&thread.sig.blocked, __ATOMIC_ACQUIRE) & bit) {
                blocked_somewhere = true;
                break;
            }
        }

        if (blocked_somewhere) {
            __atomic_fetch_or(&tg->sig.shared_pending, bit, __ATOMIC_ACQ_REL);
        }
        sync::spin_unlock_irqrestore(tg->lock, irq);
        return OK;
    }

    __atomic_fetch_or(&tg->sig.shared_pending, bit, __ATOMIC_ACQ_REL);

    if (verdict == send_verdict::FATAL) {
        // Wake one thread with the signal unblocked, leader preferred
        sched::task* target = nullptr;
        if (tg->leader &&
            !(__atomic_load_n(&tg->leader->sig.blocked, __ATOMIC_ACQUIRE) & bit)) {
            target = tg->leader;
        } else {
            for (sched::task& thread : tg->threads) {
                if (!(__atomic_load_n(&thread.sig.blocked, __ATOMIC_ACQUIRE) & bit)) {
                    target = &thread;
                    break;
                }
            }
        }
        if (target) {
            wake_for_signal(target);
        }
    }

    sync::spin_unlock_irqrestore(tg->lock, irq);
    return OK;
}

__PRIVILEGED_CODE uint32_t fatal_pending(sched::task* t) {
    sig_set_t pending = __atomic_load_n(&t->sig.pending, __ATOMIC_ACQUIRE);
    sig_set_t shared = t->group
        ? __atomic_load_n(&t->group->sig.shared_pending, __ATOMIC_ACQUIRE) : 0;

    // A pending SIGKILL is the "must terminate" marker and outranks all.
    // Report the signal that began group termination if one was recorded.
    if ((pending | shared) & sig_bit(SIGKILL)) {
        uint32_t es = t->group
            ? __atomic_load_n(&t->group->sig.exit_signal, __ATOMIC_ACQUIRE) : 0;
        return es ? es : SIGKILL;
    }

    if (!t->group) {
        return 0;
    }

    sig_set_t deliverable = (pending | shared)
        & ~__atomic_load_n(&t->sig.blocked, __ATOMIC_ACQUIRE);
    if (!deliverable) {
        return 0;
    }

    sync::irq_state irq = sync::spin_lock_irqsave(t->group->sig.lock);
    uint32_t fatal_sig = 0;
    while (deliverable) {
        uint32_t sig = static_cast<uint32_t>(__builtin_ctzll(deliverable)) + 1;
        deliverable &= deliverable - 1;

        uintptr_t handler = t->group->sig.actions[sig - 1].handler;
        if (handler == SIG_DFL && dfl_action(sig) == default_action::TERM) {
            fatal_sig = sig;
            break;
        }
    }

    sync::spin_unlock_irqrestore(t->group->sig.lock, irq);
    return fatal_sig;
}

__PRIVILEGED_CODE bool interrupt_pending(sched::task* t) {
    return t && fatal_pending(t) != 0;
}

__PRIVILEGED_CODE void die_from_signal(uint32_t sig) {
    sched::task* self = sched::current();
    sched::thread_group* tg = self->group;

    // Native kills (proc_kill) stay thread-scoped: the SIGKILL bit
    // is set without recording a group exit signal
    bool native_kill =
        (__atomic_load_n(&self->sig.pending, __ATOMIC_ACQUIRE) & sig_bit(SIGKILL)) &&
        (!tg || __atomic_load_n(&tg->sig.exit_signal, __ATOMIC_ACQUIRE) == 0);

    if (tg && !native_kill) {
        // First recorded signal wins so every group member reports it,
        // including this thread if another already recorded one
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&tg->sig.exit_signal, &expected, sig,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            sig = expected;
        }

        // A dying non-leader force-kills the leader, whose exit reaps
        // every remaining thread
        sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);
        if (tg->leader && tg->leader != self) {
            sched::force_wake_for_kill(tg->leader);
        }
        sync::spin_unlock_irqrestore(tg->lock, irq);
    }

    // The SIGKILL bit makes exit() encode a killed-by-signal wait status
    __atomic_fetch_or(&self->sig.pending, sig_bit(SIGKILL), __ATOMIC_RELEASE);
    sched::exit(static_cast<int32_t>(sig));
}

} // namespace signals
