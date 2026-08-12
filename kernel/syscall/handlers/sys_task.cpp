#include "syscall/handlers/sys_task.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "resource/handle_table.h"
#include "resource/resource.h"
#include "resource/providers/proc_provider.h"
#include "mm/uaccess.h"

// Highest value representable as a task id
constexpr int64_t TASK_ID_LIMIT = 0xFFFFFFFF;

// True if any live process belongs to the given process group
static bool group_exists(uint32_t group_id) {
    bool found = false;

    sync::irq_state irq = sched::g_task_registry.lock();
    sched::g_task_registry.for_each_locked([&](sched::task& t) {
        if (t.group &&
            __atomic_load_n(&t.group->group_id, __ATOMIC_ACQUIRE) == group_id) {
            found = true;
        }
    });
    sched::g_task_registry.unlock(irq);

    return found;
}

// Move the caller's not-yet-started child process with the given pid into
// group_id. Ownership is proven by a PROCESS handle in the caller's table.
static int64_t regroup_unstarted_child(sched::task* caller, uint32_t pid,
                                       uint32_t group_id) {
    for (uint32_t slot = 0; slot < resource::MAX_TASK_HANDLES; slot++) {
        resource::resource_object* obj = nullptr;
        int32_t rc = resource::get_handle_object(
            &caller->handles, static_cast<resource::handle_t>(slot), 0, &obj);
        if (rc != resource::HANDLE_OK) {
            continue;
        }
        if (obj->type != resource::resource_type::PROCESS) {
            resource::resource_release(obj);
            continue;
        }

        auto* pr = resource::proc_provider::get_proc_resource(obj);
        if (!pr) {
            resource::resource_release(obj);
            continue;
        }

        // pr->lock pins pr->child, so the group stays valid for the store
        sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
        sched::task* child = pr->child;
        if (!child || !child->group || child->group->pid != pid) {
            sync::spin_unlock_irqrestore(pr->lock, irq);
            resource::resource_release(obj);
            continue;
        }

        int64_t result = 0;
        if (child->state != sched::TASK_STATE_CREATED) {
            result = syscall::EACCES;
        } else {
            __atomic_store_n(&child->group->group_id, group_id,
                             __ATOMIC_RELEASE);
        }
        sync::spin_unlock_irqrestore(pr->lock, irq);
        resource::resource_release(obj);
        return result;
    }

    return syscall::ESRCH;
}

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

DEFINE_SYSCALL2(setpgid, u_pid, u_pgid) {
    sched::task* caller = sched::current();
    if (!caller->group) {
        return syscall::ESRCH;
    }

    int64_t pid  = static_cast<int64_t>(u_pid);
    int64_t pgid = static_cast<int64_t>(u_pgid);
    if (pid < 0 || pid > TASK_ID_LIMIT) {
        return syscall::ESRCH;
    }
    if (pgid < 0 || pgid > TASK_ID_LIMIT) {
        return syscall::EINVAL;
    }

    uint32_t target_pid = pid ? static_cast<uint32_t>(pid) : caller->group->pid;
    uint32_t group_id   = pgid ? static_cast<uint32_t>(pgid) : target_pid;

    // POSIX: joining a group requires it to have a live member,
    // while making a process its own group is always allowed
    if (group_id != target_pid && !group_exists(group_id)) {
        return syscall::EPERM;
    }

    if (target_pid == caller->group->pid) {
        __atomic_store_n(&caller->group->group_id, group_id, __ATOMIC_RELEASE);
        return 0;
    }

    return regroup_unstarted_child(caller, target_pid, group_id);
}

DEFINE_SYSCALL1(getpgid, u_pid) {
    sched::task* caller = sched::current();
    int64_t pid = static_cast<int64_t>(u_pid);

    if (pid == 0) {
        if (!caller->group) {
            return syscall::ESRCH;
        }
        return __atomic_load_n(&caller->group->group_id, __ATOMIC_ACQUIRE);
    }

    if (pid < 0 || pid > TASK_ID_LIMIT) {
        return syscall::ESRCH;
    }

    int64_t result = syscall::ESRCH;
    sync::irq_state irq = sched::g_task_registry.lock();
    sched::task* t = sched::g_task_registry.find_locked(static_cast<uint32_t>(pid));
    if (t && t->group) {
        result = __atomic_load_n(&t->group->group_id, __ATOMIC_ACQUIRE);
    }
    sched::g_task_registry.unlock(irq);

    return result;
}
