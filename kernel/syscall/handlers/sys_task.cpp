#include "syscall/handlers/sys_task.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/uaccess.h"
#include "mm/pmm_internal.h"
#include "mm/pmm.h"
#include "smp/smp.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

DEFINE_SYSCALL0(getpid) {
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
    (void)u_rem;

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
    sched::sleep_ns(ns);
    return 0;
}

DEFINE_SYSCALL1(sysinfo, u_info) {
    if (u_info == 0) return syscall::EFAULT;

    // Linux struct sysinfo layout (x86_64 and aarch64 are identical)
    struct kernel_sysinfo {
        uint64_t uptime;
        uint64_t loads[3];
        uint64_t totalram;
        uint64_t freeram;
        uint64_t sharedram;
        uint64_t bufferram;
        uint64_t totalswap;
        uint64_t freeswap;
        uint16_t procs;
        uint16_t pad;
        uint32_t pad2;
        uint64_t totalhigh;
        uint64_t freehigh;
        uint32_t mem_unit;
        char _pad[256];
    };

    kernel_sysinfo si;
    string::memset(&si, 0, sizeof(si));

    uint64_t total_pages = 0;
    uint64_t free_pages = 0;
    {
        bool was = dynpriv::is_elevated();
        if (!was) dynpriv::elevate();
        for (size_t i = 0; i < static_cast<size_t>(pmm::zone_id::COUNT); i++) {
            total_pages += pmm::g_pmm.zones[i].total_pages;
            free_pages += pmm::g_pmm.zones[i].free_pages;
        }
        if (!was) dynpriv::lower();
    }

    si.totalram = total_pages * pmm::PAGE_SIZE;
    si.freeram = free_pages * pmm::PAGE_SIZE;
    si.mem_unit = 1;
    si.procs = static_cast<uint16_t>(smp::online_count());

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_info), &si, sizeof(si));
    if (rc != mm::uaccess::OK) return syscall::EFAULT;
    return 0;
}

DEFINE_SYSCALL3(sched_getaffinity, pid, cpusetsize, u_mask) {
    (void)pid; // only support current process (pid=0)

    if (u_mask == 0 || cpusetsize == 0) return syscall::EINVAL;

    uint32_t ncpus = smp::online_count();
    if (ncpus == 0) ncpus = 1;

    // Build a bitmask with ncpus bits set
    uint8_t mask[128];
    string::memset(mask, 0, sizeof(mask));
    for (uint32_t i = 0; i < ncpus && i < sizeof(mask) * 8; i++) {
        mask[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }

    size_t copy_len = cpusetsize < sizeof(mask) ? cpusetsize : sizeof(mask);
    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_mask), mask, copy_len);
    if (rc != mm::uaccess::OK) return syscall::EFAULT;
    return static_cast<int64_t>(copy_len);
}
