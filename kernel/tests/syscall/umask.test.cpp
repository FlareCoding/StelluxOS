#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_umask.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(umask_syscall);

// Swap semantics are verified live from userland, since kernel test
// tasks have no thread group to hold the mask.

TEST(umask_syscall, groupless_caller_is_esrch) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_umask(022, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);
}
