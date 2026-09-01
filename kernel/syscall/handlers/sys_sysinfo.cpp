#include "syscall/handlers/sys_sysinfo.h"
#include "mm/uaccess.h"
#include "mm/pmm.h"
#include "clock/clock.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "common/string.h"

// Layout matches the Linux uapi struct that musl passes through verbatim.
struct linux_sysinfo {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char     reserved[20 - 2 * sizeof(uint64_t) - sizeof(uint32_t)];
};

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

DEFINE_SYSCALL1(sysinfo, u_info) {
    if (u_info == 0) {
        return syscall::EFAULT;
    }

    linux_sysinfo kinfo;
    string::memset(&kinfo, 0, sizeof(kinfo));

    kinfo.uptime = static_cast<int64_t>(clock::now_ns() / NS_PER_SEC);
    kinfo.totalram = pmm::total_page_count() * pmm::PAGE_SIZE;
    kinfo.freeram = pmm::free_page_count() * pmm::PAGE_SIZE;
    kinfo.mem_unit = 1;

    uint16_t procs = 0;
    sync::irq_state irq = sched::g_task_registry.lock();
    sched::g_task_registry.for_each_locked([&](sched::task&) {
        procs++;
    });

    sched::g_task_registry.unlock(irq);
    kinfo.procs = procs;

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(u_info), &kinfo, sizeof(kinfo));

    if (rc != mm::uaccess::OK) {
        return syscall::EFAULT;
    }

    return 0;
}
