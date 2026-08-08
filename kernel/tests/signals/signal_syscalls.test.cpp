#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "signals/signal.h"
#include "syscall/handlers/sys_signal.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(signal_syscalls);

// musl always passes sigsetsize 8, anything else must be rejected
TEST(signal_syscalls, rejects_wrong_sigsetsize) {
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_rt_sigaction(signals::SIGINT, 0, 0, 4, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);

    RUN_ELEVATED({ rc = sys_rt_sigprocmask(signals::SIG_BLOCK, 0, 0, 16, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);

    RUN_ELEVATED({ rc = sys_rt_sigpending(0, 4, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);
}

// Kernel tasks have no thread group, so no action table to modify
TEST(signal_syscalls, sigaction_requires_a_process) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_rt_sigaction(signals::SIGINT, 0, 0, 8, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);
}
