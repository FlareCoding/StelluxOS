#include "syscall/handlers/sys_rusage.h"
#include "mm/uaccess.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "timer/timer.h"
#include "common/string.h"

// Layout matches the Linux uapi struct that musl passes through verbatim.
struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct linux_rusage {
    linux_timeval ru_utime;
    linux_timeval ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};

constexpr int64_t RUSAGE_SELF     = 0;
constexpr int64_t RUSAGE_CHILDREN = -1;
constexpr int64_t RUSAGE_THREAD   = 1;

constexpr uint64_t USEC_PER_SEC = 1000000ULL;

static void ticks_to_timeval(uint64_t ticks, uint32_t hz, linux_timeval* tv) {
    uint64_t usec = ticks * (USEC_PER_SEC / hz);
    tv->tv_sec = static_cast<int64_t>(usec / USEC_PER_SEC);
    tv->tv_usec = static_cast<int64_t>(usec % USEC_PER_SEC);
}

DEFINE_SYSCALL2(getrusage, u_who, u_usage) {
    int64_t who = static_cast<int64_t>(u_who);
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
        return syscall::EINVAL;
    }

    if (u_usage == 0) {
        return syscall::EFAULT;
    }

    sched::task* current = sched::current();
    if (!current) {
        return syscall::EIO;
    }

    linux_rusage kusage;
    string::memset(&kusage, 0, sizeof(kusage));

    // Ticks accumulate while a task is current, with no user/kernel time
    // split, so all CPU time reports as user time. Children report zero
    // since no accounting survives a child's exit.
    uint64_t ticks = 0;
    if (who == RUSAGE_THREAD) {
        ticks = current->run_ticks.load_relaxed();
    } else if (who == RUSAGE_SELF) {
        sync::irq_state irq = sched::g_task_registry.lock();
        sched::g_task_registry.for_each_locked([&](sched::task& t) {
            if (t.group && current->group && t.group->pid == current->group->pid) {
                ticks += t.run_ticks.load_relaxed();
            }
        });
        sched::g_task_registry.unlock(irq);
    }
    ticks_to_timeval(ticks, timer::tick_hz(), &kusage.ru_utime);

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_usage), &kusage, sizeof(kusage));
    if (rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}
