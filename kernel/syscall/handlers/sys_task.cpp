#include "syscall/handlers/sys_task.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"

DEFINE_SYSCALL0(getpid) {
    sched::task* t = sched::current();
    return static_cast<int64_t>(t->group ? t->group->pid : t->tid);
}

DEFINE_SYSCALL0(gettid) {
    return static_cast<int64_t>(sched::current()->tid);
}

DEFINE_SYSCALL0(getuid)  { return 0; }
DEFINE_SYSCALL0(geteuid) { return 0; }
DEFINE_SYSCALL0(getgid)  { return 0; }
DEFINE_SYSCALL0(getegid) { return 0; }

DEFINE_SYSCALL0(set_tid_address) {
    return static_cast<int64_t>(sched::current()->tid);
}

DEFINE_SYSCALL1(exit, status) {
    sched::exit(static_cast<int>(status));
    __builtin_unreachable();
}

DEFINE_SYSCALL1(exit_group, status) {
    sched::exit(static_cast<int>(status));
    __builtin_unreachable();
}

DEFINE_SYSCALL2(nanosleep, u_req, u_rem) {
    struct kernel_timespec {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    if (u_req == 0) {
        return syscall::EFAULT;
    }

    kernel_timespec ts;
    int32_t rc = mm::uaccess::copy_from_user(
        &ts, reinterpret_cast<const void*>(u_req), sizeof(ts));
    if (rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    if (ts.tv_nsec < 0 || ts.tv_nsec > 999999999 || ts.tv_sec < 0) {
        return syscall::EINVAL;
    }

    uint64_t ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                + static_cast<uint64_t>(ts.tv_nsec);
    uint64_t rem_ns = sched::sleep_ns(ns);
    if (rem_ns == 0) {
        return 0;
    }

    if (u_rem != 0) {
        kernel_timespec rem = {
            static_cast<int64_t>(rem_ns / 1000000000ULL),
            static_cast<int64_t>(rem_ns % 1000000000ULL),
        };
        if (mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(u_rem), &rem, sizeof(rem)) != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }
    return syscall::EINTR;
}
