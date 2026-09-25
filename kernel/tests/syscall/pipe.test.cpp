#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_pipe.h"
#include "resource/resource.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(pipe_syscall);

static uint32_t handle_flags_of(sched::task* task, int32_t h) {
    uint32_t flags = 0;
    resource::get_handle_flags(task->handles, h, &flags);
    return flags;
}

// Creates a pipe the way userland does, with the two handles landing in the page
static int64_t pipe2_into(user_page& page, uint32_t flags) {
    user_space_scope scope(page.ctx);
    return sys_pipe2(page.addr, flags, 0, 0, 0, 0);
}

TEST(pipe_syscall, cloexec_lands_on_both_ends) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(pipe2_into(page, fs::O_CLOEXEC), 0);

    int32_t* fds = page.at<int32_t>(0);
    EXPECT_EQ(handle_flags_of(task, fds[0]), resource::RESOURCE_HANDLE_CLOEXEC);
    EXPECT_EQ(handle_flags_of(task, fds[1]), resource::RESOURCE_HANDLE_CLOEXEC);

    EXPECT_EQ(resource::close(task, fds[0]), resource::OK);
    EXPECT_EQ(resource::close(task, fds[1]), resource::OK);
}

TEST(pipe_syscall, nonblock_and_cloexec_combine) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(pipe2_into(page, fs::O_NONBLOCK | fs::O_CLOEXEC), 0);

    int32_t* fds = page.at<int32_t>(0);
    uint32_t expected = fs::O_NONBLOCK | resource::RESOURCE_HANDLE_CLOEXEC;
    EXPECT_EQ(handle_flags_of(task, fds[0]), expected);
    EXPECT_EQ(handle_flags_of(task, fds[1]), expected);

    EXPECT_EQ(resource::close(task, fds[0]), resource::OK);
    EXPECT_EQ(resource::close(task, fds[1]), resource::OK);
}

TEST(pipe_syscall, unsupported_flags_are_refused) {
    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(pipe2_into(page, fs::O_APPEND), syscall::EINVAL);
}
