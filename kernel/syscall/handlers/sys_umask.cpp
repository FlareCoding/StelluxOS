#include "syscall/handlers/sys_umask.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/spinlock.h"

constexpr uint32_t MODE_BITS = 0777;

DEFINE_SYSCALL1(umask, u_mask) {
    sched::task* current = sched::current();
    if (!current || !current->group) {
        return syscall::ESRCH;
    }

    // The filesystem synthesizes permissions and consumes no creation
    // mode yet, so the mask's observable behavior is the swap itself.
    sched::thread_group* group = current->group;
    uint32_t new_mask = static_cast<uint32_t>(u_mask) & MODE_BITS;

    sync::spin_lock(group->lock);
    uint32_t old_mask = group->umask;
    group->umask = new_mask;
    sync::spin_unlock(group->lock);

    return static_cast<int64_t>(old_mask);
}
