#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_prlimit.h"
#include "sched/rlimits.h"
#include "resource/handle_table.h"
#include "mm/vma.h"
#include "mm/pmm.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(prlimit_syscall);

// Get and set round trips are verified live from userland, since kernel
// test tasks have no thread group and no user address space.

TEST(prlimit_syscall, rejects_out_of_range_resource) {
    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_prlimit64(0, sched::RLIMIT_COUNT, 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::EINVAL);
}

TEST(prlimit_syscall, groupless_caller_is_esrch) {
    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_prlimit64(0, sched::RLIMIT_STACK, 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::ESRCH);
}

TEST(prlimit_syscall, default_limits_reflect_kernel_constants) {
    sched::rlimit_pair limits[sched::RLIMIT_COUNT];
    sched::init_default_rlimits(limits);

    EXPECT_EQ(limits[sched::RLIMIT_NOFILE].soft,
              static_cast<uint64_t>(resource::MAX_TASK_HANDLES));
    EXPECT_EQ(limits[sched::RLIMIT_STACK].soft,
              mm::USER_STACK_MAX_PAGES * pmm::PAGE_SIZE);
    EXPECT_EQ(limits[0].soft, sched::RLIM_INFINITY);
}
