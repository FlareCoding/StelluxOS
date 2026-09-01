#include "syscall/handlers/sys_prlimit.h"
#include "mm/uaccess.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/spinlock.h"

// Soft and hard limit pair in the layout musl passes through verbatim
struct linux_rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

DEFINE_SYSCALL4(prlimit64, u_pid, u_resource, u_new, u_old) {
    if (u_resource >= sched::RLIMIT_COUNT) {
        return syscall::EINVAL;
    }

    sched::task* current = sched::current();
    if (!current || !current->group) {
        return syscall::ESRCH;
    }

    // Limits are process private, so only the caller's own may be touched
    if (u_pid != 0 && u_pid != current->group->pid) {
        return syscall::EPERM;
    }

    linux_rlimit64 knew;
    if (u_new != 0) {
        int32_t rc = mm::uaccess::copy_from_user(
            &knew, reinterpret_cast<const void*>(u_new), sizeof(knew));
        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }

        if (knew.rlim_cur > knew.rlim_max) {
            return syscall::EINVAL;
        }
    }

    sched::thread_group* group = current->group;
    linux_rlimit64 kold;

    sync::spin_lock(group->lock);
    kold.rlim_cur = group->rlimits[u_resource].soft;
    kold.rlim_max = group->rlimits[u_resource].hard;
    if (u_new != 0) {
        group->rlimits[u_resource] = { knew.rlim_cur, knew.rlim_max };
    }
    sync::spin_unlock(group->lock);

    if (u_old != 0) {
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_old), &kold, sizeof(kold));
        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    return 0;
}
