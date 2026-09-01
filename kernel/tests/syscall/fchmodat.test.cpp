#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_fd.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(fchmodat_syscall);

// Path resolution behavior is verified live from userland, since kernel
// test tasks have no user address space to hold the path string.

TEST(fchmodat_syscall, rejects_unreadable_path_pointer) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_fchmodat(0, 0, 0644, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EFAULT);
}
