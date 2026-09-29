#include "syscall/handlers/sys_clock.h"
#include "syscall/syscall_table.h"
#include "clock/clock.h"
#include "mm/uaccess.h"
#include "sched/sched.h"
#include "sched/task.h"

constexpr uint64_t CLOCK_REALTIME           = 0;
constexpr uint64_t CLOCK_MONOTONIC          = 1;
constexpr uint64_t CLOCK_PROCESS_CPUTIME_ID = 2;
constexpr uint64_t CLOCK_THREAD_CPUTIME_ID  = 3;
constexpr uint64_t CLOCK_MONOTONIC_RAW      = 4;
constexpr uint64_t CLOCK_REALTIME_COARSE    = 5;
constexpr uint64_t CLOCK_MONOTONIC_COARSE   = 6;
constexpr uint64_t CLOCK_BOOTTIME           = 7;

constexpr uint64_t NS_PER_SEC = 1000000000ULL;
constexpr int64_t COARSE_RES_NS = 10000000; // 10 ms at 100 Hz tick

// A negative clock id names a task's CPU clock: the complement of the task's
// tid or pid sits above the low three bits, which select the clock
constexpr uint32_t CPU_CLOCK_ID_SHIFT   = 3;
constexpr int32_t  CPU_CLOCK_THREAD_BIT = 4; // a thread's clock rather than its process's
constexpr int32_t  CPU_CLOCK_KIND_MASK  = 3; // user plus system, user, or scheduler time
constexpr int32_t  CPU_CLOCK_KIND_COUNT = 3;

namespace {

struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct kernel_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct kernel_timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};

} // anonymous namespace

static uint64_t get_monotonic_ns() {
    return clock::now_ns();
}

/**
 * Reads the CPU clock a clock id names. A thread's clock is readable only
 * from its own process, a process's clock is named by its leader's id, and
 * an id of zero names the caller. Every kind reads the same total, since
 * CPU time has no user and system split.
 * @return False when the id names no clock the caller may read.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool read_cpu_clock_ns(uint64_t clock_id, uint64_t* out_ns) {
    sched::task* self = sched::current();

    if (clock_id == CLOCK_THREAD_CPUTIME_ID) {
        *out_ns = sched::read_task_cpu_time_ns(self);
        return true;
    }

    if (clock_id == CLOCK_PROCESS_CPUTIME_ID) {
        *out_ns = sched::read_process_cpu_time_ns(self);
        return true;
    }

    int32_t encoded = static_cast<int32_t>(clock_id);
    if (encoded >= 0 || (encoded & CPU_CLOCK_KIND_MASK) >= CPU_CLOCK_KIND_COUNT) {
        return false;
    }

    bool is_thread_clock = (encoded & CPU_CLOCK_THREAD_BIT) != 0;
    uint32_t id = static_cast<uint32_t>(~(encoded >> CPU_CLOCK_ID_SHIFT));
    if (id == 0) {
        *out_ns = is_thread_clock ? sched::read_task_cpu_time_ns(self) : sched::read_process_cpu_time_ns(self);
        return true;
    }

    rc::strong_ref<sched::task> target = sched::task_ref_by_tid(id);
    if (!target) {
        return false;
    }

    if (is_thread_clock) {
        bool in_own_process = target.ptr() == self || (self->group && target->group == self->group);
        if (!in_own_process) {
            return false;
        }

        *out_ns = sched::read_task_cpu_time_ns(target.ptr());
        return true;
    }

    bool names_process = target.ptr() == self || (target->group && target->group->pid == id);
    if (!names_process) {
        return false;
    }

    *out_ns = sched::read_process_cpu_time_ns(target.ptr());
    return true;
}

DEFINE_SYSCALL2(clock_gettime, clock_id, u_tp) {
    if (u_tp == 0) {
        return syscall::EFAULT;
    }

    uint64_t ns;
    switch (clock_id) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        ns = get_monotonic_ns();
        break;
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        if (clock::boot_realtime_ns() == 0) {
            return syscall::EINVAL;
        }

        ns = clock::realtime_ns();
        break;
    default:
        if (!read_cpu_clock_ns(clock_id, &ns)) {
            return syscall::EINVAL;
        }

        break;
    }

    kernel_timespec ts;
    ts.tv_sec = static_cast<int64_t>(ns / NS_PER_SEC);
    ts.tv_nsec = static_cast<int64_t>(ns % NS_PER_SEC);

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_tp), &ts, sizeof(ts));
    if (rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}

DEFINE_SYSCALL2(clock_getres, clock_id, u_tp) {
    int64_t res_ns;
    switch (clock_id) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME:
    case CLOCK_REALTIME:
        res_ns = 1;
        break;
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_REALTIME_COARSE:
        res_ns = COARSE_RES_NS;
        break;
    default: {
        uint64_t unused_ns = 0;
        if (!read_cpu_clock_ns(clock_id, &unused_ns)) {
            return syscall::EINVAL;
        }

        res_ns = 1;
        break;
    }
    }

    if (u_tp != 0) {
        kernel_timespec res;
        res.tv_sec = 0;
        res.tv_nsec = res_ns;

        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_tp), &res, sizeof(res));
        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    return 0;
}

DEFINE_SYSCALL2(gettimeofday, u_tv, u_tz) {
    if (u_tv != 0) {
        if (clock::boot_realtime_ns() == 0) {
            return syscall::EINVAL;
        }

        uint64_t ns = clock::realtime_ns();
        kernel_timeval tv;
        tv.tv_sec = static_cast<int64_t>(ns / NS_PER_SEC);
        tv.tv_usec = static_cast<int64_t>((ns % NS_PER_SEC) / 1000);

        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_tv), &tv, sizeof(tv));
        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    if (u_tz != 0) {
        kernel_timezone tz = {0, 0};
        int32_t rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(u_tz), &tz, sizeof(tz));
        if (rc != mm::uaccess::OK) {
            return syscall::EFAULT;
        }
    }

    return 0;
}
