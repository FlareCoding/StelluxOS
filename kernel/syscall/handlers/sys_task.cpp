#include "syscall/handlers/sys_task.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "sync/atomic.h"
#include "resource/handle_table.h"
#include "resource/resource.h"
#include "resource/providers/proc_provider.h"
#include "mm/uaccess.h"

// Highest value representable as a task id
constexpr int64_t TASK_ID_LIMIT = 0xFFFFFFFF;

// Clone flag bits recognized by the thread only clone path
constexpr uint64_t CLONE_VM             = 0x00000100;
constexpr uint64_t CLONE_FS             = 0x00000200;
constexpr uint64_t CLONE_FILES          = 0x00000400;
constexpr uint64_t CLONE_SIGHAND        = 0x00000800;
constexpr uint64_t CLONE_THREAD         = 0x00010000;
constexpr uint64_t CLONE_SYSVSEM        = 0x00040000;
constexpr uint64_t CLONE_SETTLS         = 0x00080000;
constexpr uint64_t CLONE_PARENT_SETTID  = 0x00100000;
constexpr uint64_t CLONE_CHILD_CLEARTID = 0x00200000;
constexpr uint64_t CLONE_DETACHED       = 0x00400000;
constexpr uint64_t CLONE_CHILD_SETTID   = 0x01000000;

// The exit signal lives in the low byte of the flags word
constexpr uint64_t CLONE_CSIGNAL_MASK   = 0x000000FF;

// The thread bundle musl requests from pthread_create. Anything else is
// fork shaped and rejected, the process model spawns fresh processes.
constexpr uint64_t CLONE_REQUIRED_BUNDLE =
    CLONE_VM | CLONE_FS | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
constexpr uint64_t CLONE_SUPPORTED_EXTRAS =
    CLONE_FILES | CLONE_SETTLS | CLONE_PARENT_SETTID |
    CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | CLONE_DETACHED;

// True if any live process belongs to the given process group
static bool group_exists(uint32_t group_id) {
    bool found = false;

    sync::irq_state irq = sched::g_task_registry.lock();
    sched::g_task_registry.for_each_locked([&](sched::task& t) {
        if (t.group &&
            t.group->group_id.load_acquire() == group_id) {
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
            caller->handles, static_cast<resource::handle_t>(slot), 0, &obj);
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
        if (child->state.load_relaxed() != sched::TASK_STATE_CREATED) {
            result = syscall::EACCES;
        } else {
            child->group->group_id.store_release(group_id);
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

DEFINE_SYSCALL1(set_tid_address, u_tidptr) {
    sched::task* t = sched::current();
    t->clear_child_tid = static_cast<uintptr_t>(u_tidptr);
    return static_cast<int64_t>(t->tid);
}

// x86_64 passes (flags, stack, ptid, ctid, tls),
// aarch64 passes (flags, stack, ptid, tls, ctid)
#if defined(__x86_64__)
DEFINE_SYSCALL5(clone, u_flags, u_stack, u_ptid, u_ctid, u_tls) {
#else
DEFINE_SYSCALL5(clone, u_flags, u_stack, u_ptid, u_tls, u_ctid) {
#endif
    uint64_t flags = static_cast<uint64_t>(u_flags);

    // Internally inconsistent flag combinations come first
    if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) {
        return syscall::EINVAL;
    }

    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) {
        return syscall::EINVAL;
    }

    if ((flags & CLONE_THREAD) && (flags & CLONE_CSIGNAL_MASK) != 0) {
        return syscall::EINVAL;
    }

    // Well formed requests outside the thread bundle are fork shaped
    // and unsupported by design
    if ((flags & CLONE_REQUIRED_BUNDLE) != CLONE_REQUIRED_BUNDLE) {
        return syscall::ENOSYS;
    }

    if ((flags & ~(CLONE_REQUIRED_BUNDLE | CLONE_SUPPORTED_EXTRAS)) != 0) {
        return syscall::ENOSYS;
    }

    if (u_stack == 0) {
        return syscall::EINVAL;
    }

    sched::task* caller = sched::current();

    sched::task* child = sched::clone_user_thread(
        caller,
        static_cast<uintptr_t>(u_stack),
        static_cast<uintptr_t>(u_tls),
        (flags & CLONE_SETTLS) != 0,
        (flags & CLONE_FILES) != 0);

    if (!child) {
        return syscall::ENOMEM;
    }

    uint32_t tid = child->tid;

    if (flags & CLONE_PARENT_SETTID) {
        if (mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(u_ptid), &tid, sizeof(tid))
                != mm::uaccess::OK) {
            resource::proc_provider::destroy_unstarted_task(child);
            return syscall::EFAULT;
        }
    }

    if (flags & CLONE_CHILD_SETTID) {
        if (mm::uaccess::copy_to_user(
                reinterpret_cast<void*>(u_ctid), &tid, sizeof(tid))
                != mm::uaccess::OK) {
            resource::proc_provider::destroy_unstarted_task(child);
            return syscall::EFAULT;
        }
    }

    if (flags & CLONE_CHILD_CLEARTID) {
        child->clear_child_tid = static_cast<uintptr_t>(u_ctid);
    }

    sched::enqueue(child);

    return static_cast<int64_t>(tid);
}

DEFINE_SYSCALL0(sched_yield) {
    sched::yield();
    return 0;
}

DEFINE_SYSCALL1(exit, status) {
    sched::exit(static_cast<int>(status));
    __builtin_unreachable();
}

DEFINE_SYSCALL1(exit_group, status) {
    sched::task* self = sched::current();
    sched::thread_group* tg = self->group;

    // Only a POSIX thread takes the whole process down with it, a
    // native thread exits alone and stays joinable by its creator
    if (tg && (self->exec.flags & sched::TASK_FLAG_POSIX_THREAD)) {
        // First recorded status wins so every member reports the same
        // exit code, encoded as a normal wait status with bit 31 set
        uint32_t packed = 0x80000000u |
            ((static_cast<uint32_t>(status) & 0xFF) << 8);

        uint32_t expected = 0;
        if (!tg->group_exit_status.cmpxchg_strong_acq_rel(expected, packed)) {
            // Acquire pairs with the winning store that recorded the status.
            sync::atomic_fence_acquire();
        }

        // A non leader forces the leader down, the leader's exit then
        // reaps every remaining thread including this one
        sync::irq_state irq = sync::spin_lock_irqsave(tg->lock);

        if (tg->leader && tg->leader != self) {
            sched::force_wake_for_kill(tg->leader);
        }

        sync::spin_unlock_irqrestore(tg->lock, irq);
    }

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
        caller->group->group_id.store_release(group_id);
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

        return caller->group->group_id.load_acquire();
    }

    if (pid < 0 || pid > TASK_ID_LIMIT) {
        return syscall::ESRCH;
    }

    int64_t result = syscall::ESRCH;
    sync::irq_state irq = sched::g_task_registry.lock();
    sched::task* t = sched::g_task_registry.find_locked(static_cast<uint32_t>(pid));
    if (t && t->group) {
        result = t->group->group_id.load_acquire();
    }
    sched::g_task_registry.unlock(irq);

    return result;
}
