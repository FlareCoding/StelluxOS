#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_sysinfo.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(sysinfo_syscall);

// Content is verified live from userland, since kernel test tasks have
// no user address space for the copy-out path to target.

TEST(sysinfo_syscall, rejects_null_buffer) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_sysinfo(0, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EFAULT);
}

TEST(sysinfo_syscall, rejects_kernel_task_without_user_space) {
    uint8_t buf[4];

    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_sysinfo(reinterpret_cast<uint64_t>(buf), 0, 0, 0, 0, 0);
    });

    EXPECT_EQ(rc, syscall::EFAULT);
}
