#include "syscall/handlers/sys_signal.h"
#include "arch/arch_signal.h"
#include "signals/signal.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "mm/uaccess.h"

// musl passes sigsetsize = _NSIG/8 = 8 (64-bit sigset)
static constexpr uint64_t SIGSET_SIZE = 8;

// Highest value representable as a task or group id
static constexpr int64_t TASK_ID_LIMIT = 0xFFFFFFFF;

static int64_t map_send_error(int32_t rc) {
    switch (rc) {
        case signals::OK:       return 0;
        case signals::ERR_PERM: return syscall::EPERM;
        default:                return syscall::EINVAL;
    }
}

// Send sig to every process in the group, where sig 0 only probes existence
static int64_t kill_process_group(uint32_t group_id, uint32_t sig) {
    return signals::send_to_group_id(group_id, sig) == signals::OK
        ? 0 : syscall::ESRCH;
}

// Thread-directed send shared by tkill and tgkill, tgid 0 skips the pair check
static int64_t send_to_thread(uint32_t tgid, uint32_t tid, uint32_t sig) {
    int64_t result = syscall::ESRCH;

    sync::irq_state irq = sched::g_task_registry.lock();
    sched::task* t = sched::g_task_registry.find_locked(tid);
    if (t && (tgid == 0 || (t->group && t->group->pid == tgid))) {
        if (sig != 0) {
            result = map_send_error(signals::send_to_task(t, sig));
        } else {
            // The null probe reports the same permission gate a send would
            bool denied = (t->exec.flags &
                           (sched::TASK_FLAG_KERNEL | sched::TASK_FLAG_IDLE)) ||
                          !t->group;
            result = denied ? syscall::EPERM : 0;
        }
    }
    sched::g_task_registry.unlock(irq);

    return result;
}

DEFINE_SYSCALL4(rt_sigaction, signum, u_act, u_oldact, sigsetsize) {
    if (sigsetsize != SIGSET_SIZE) {
        return syscall::EINVAL;
    }

    sched::task* t = sched::current();
    if (!t || !t->group) {
        return syscall::EINVAL;
    }

    signals::k_sigaction kact;
    if (u_act != 0) {
        if (mm::uaccess::copy_from_user(
                &kact, reinterpret_cast<const void*>(u_act),
                sizeof(kact)) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    signals::k_sigaction kold;
    int32_t rc = signals::set_action(
        t->group, static_cast<uint32_t>(signum),
        u_act != 0 ? &kact : nullptr,
        u_oldact != 0 ? &kold : nullptr);
    if (rc != signals::OK) {
        return syscall::EINVAL;
    }

    if (u_oldact != 0) {
        if (mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(u_oldact), &kold,
                sizeof(kold)) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }
    return 0;
}

DEFINE_SYSCALL4(rt_sigprocmask, how, u_set, u_oldset, sigsetsize) {
    if (sigsetsize != SIGSET_SIZE) {
        return syscall::EINVAL;
    }

    sched::task* t = sched::current();
    if (!t) {
        return syscall::EINVAL;
    }

    signals::sig_set_t kset = 0;
    if (u_set != 0) {
        if (mm::uaccess::copy_from_user(
                &kset, reinterpret_cast<const void*>(u_set),
                sizeof(kset)) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    signals::sig_set_t kold = 0;
    int32_t rc = signals::set_blocked(
        t, static_cast<uint32_t>(how),
        u_set != 0 ? &kset : nullptr,
        u_oldset != 0 ? &kold : nullptr);
    if (rc != signals::OK) {
        return syscall::EINVAL;
    }

    if (u_oldset != 0) {
        if (mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(u_oldset), &kold,
                sizeof(kold)) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }
    return 0;
}

DEFINE_SYSCALL2(rt_sigpending, u_set, sigsetsize) {
    if (sigsetsize != SIGSET_SIZE) {
        return syscall::EINVAL;
    }

    sched::task* t = sched::current();
    if (!t) {
        return syscall::EINVAL;
    }

    signals::sig_set_t pending = signals::pending_blocked_set(t);
    if (mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_set), &pending,
            sizeof(pending)) != mm::uaccess::OK) {
        return syscall::EFAULT;
    }
    return 0;
}

DEFINE_SYSCALL0(rt_sigreturn) {
    // Returns the restored context's saved result value, so the normal
    // result write completes the restore instead of clobbering it
    return arch::restore_signal_context();
}

DEFINE_SYSCALL2(kill, u_pid, u_sig) {
    if (u_sig > signals::NSIG) {
        return syscall::EINVAL;
    }
    uint32_t sig = static_cast<uint32_t>(u_sig);
    int64_t pid  = static_cast<int64_t>(u_pid);

    // Broadcast kill is not supported
    if (pid == -1) {
        return syscall::EINVAL;
    }

    if (pid > 0) {
        if (pid > TASK_ID_LIMIT) {
            return syscall::ESRCH;
        }

        // Any thread id resolves to its containing process (kill semantics
        // on Linux), and the signal is delivered process-wide
        int64_t result = syscall::ESRCH;
        sync::irq_state irq = sched::g_task_registry.lock();
        sched::task* t =
            sched::g_task_registry.find_locked(static_cast<uint32_t>(pid));
        if (t && t->group) {
            result = sig ? map_send_error(signals::send_to_group(t->group, sig))
                         : 0;
        }
        sched::g_task_registry.unlock(irq);
        return result;
    }

    // pid 0 targets the caller's group, below -1 the group named by -pid
    uint32_t group_id = 0;
    if (pid == 0) {
        sched::task* caller = sched::current();
        if (!caller->group) {
            return syscall::ESRCH;
        }
        group_id = __atomic_load_n(&caller->group->group_id, __ATOMIC_ACQUIRE);
    } else {
        if (pid < -TASK_ID_LIMIT) {
            return syscall::ESRCH;
        }
        group_id = static_cast<uint32_t>(-pid);
    }

    return kill_process_group(group_id, sig);
}

DEFINE_SYSCALL2(tkill, u_tid, u_sig) {
    if (u_sig > signals::NSIG) {
        return syscall::EINVAL;
    }

    int64_t tid = static_cast<int64_t>(u_tid);
    if (tid <= 0 || tid > TASK_ID_LIMIT) {
        return syscall::EINVAL;
    }

    return send_to_thread(0, static_cast<uint32_t>(tid),
                          static_cast<uint32_t>(u_sig));
}

DEFINE_SYSCALL3(tgkill, u_tgid, u_tid, u_sig) {
    if (u_sig > signals::NSIG) {
        return syscall::EINVAL;
    }

    int64_t tgid = static_cast<int64_t>(u_tgid);
    int64_t tid  = static_cast<int64_t>(u_tid);
    if (tgid <= 0 || tid <= 0 || tgid > TASK_ID_LIMIT || tid > TASK_ID_LIMIT) {
        return syscall::EINVAL;
    }

    return send_to_thread(static_cast<uint32_t>(tgid),
                          static_cast<uint32_t>(tid),
                          static_cast<uint32_t>(u_sig));
}
