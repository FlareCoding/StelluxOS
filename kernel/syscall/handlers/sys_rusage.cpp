#include "syscall/handlers/sys_rusage.h"
#include "mm/uaccess.h"
#include "sched/sched.h"
#include "sched/task.h"
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

constexpr uint64_t NS_PER_SEC  = 1000000000ULL;
constexpr uint64_t NS_PER_USEC = 1000ULL;

static void ns_to_timeval(uint64_t ns, linux_timeval* tv) {
    tv->tv_sec = static_cast<int64_t>(ns / NS_PER_SEC);
    tv->tv_usec = static_cast<int64_t>((ns % NS_PER_SEC) / NS_PER_USEC);
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

    // CPU time has no user/kernel split, so all of it reports as user time.
    // Children report zero since no accounting survives a child's exit.
    uint64_t cpu_time_ns = 0;
    if (who == RUSAGE_THREAD) {
        cpu_time_ns = sched::read_task_cpu_time_ns(current);
    } else if (who == RUSAGE_SELF) {
        cpu_time_ns = current->group ? sched::read_group_cpu_time_ns(current->group)
                                     : sched::read_task_cpu_time_ns(current);
    }

    ns_to_timeval(cpu_time_ns, &kusage.ru_utime);

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_usage), &kusage, sizeof(kusage));
    if (rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}
