#include "syscall/handlers/sys_clock.h"
#include "syscall/syscall_table.h"
#include "clock/clock.h"
#include "mm/uaccess.h"

namespace {

struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

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

struct kernel_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct kernel_timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};

uint64_t get_monotonic_ns() {
    return clock::now_ns();
}

uint64_t get_realtime_ns() {
    return clock::boot_realtime_ns() + clock::now_ns();
}

} // anonymous namespace

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
    case CLOCK_PROCESS_CPUTIME_ID:
        ns = get_monotonic_ns();
        break;
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        if (clock::boot_realtime_ns() == 0) {
            return syscall::EINVAL;
        }
        ns = get_realtime_ns();
        break;
    case CLOCK_THREAD_CPUTIME_ID:
        return syscall::EINVAL;
    default:
        return syscall::EINVAL;
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
    case CLOCK_PROCESS_CPUTIME_ID:
        res_ns = 1;
        break;
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_REALTIME_COARSE:
        res_ns = COARSE_RES_NS;
        break;
    case CLOCK_THREAD_CPUTIME_ID:
        return syscall::EINVAL;
    default:
        return syscall::EINVAL;
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
        uint64_t ns = get_realtime_ns();
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
