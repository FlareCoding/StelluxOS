#include "syscall/handlers/sys_signal.h"
#include "signals/signal.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"

// musl passes sigsetsize = _NSIG/8 = 8 (64-bit sigset)
static constexpr uint64_t SIGSET_SIZE = 8;

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
