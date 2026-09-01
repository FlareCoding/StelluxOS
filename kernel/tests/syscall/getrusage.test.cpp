#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_rusage.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(getrusage_syscall);

// Content is verified live from userland, since kernel test tasks have
// no user address space for the copy-out path to target.

TEST(getrusage_syscall, rejects_invalid_who) {
    uint8_t buf[4];

    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_getrusage(2, reinterpret_cast<uint64_t>(buf), 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::EINVAL);

    RUN_ELEVATED({
        rc = sys_getrusage(static_cast<uint64_t>(-2ll),
                           reinterpret_cast<uint64_t>(buf), 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::EINVAL);
}

TEST(getrusage_syscall, rejects_null_buffer) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_getrusage(0, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EFAULT);
}
