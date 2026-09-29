#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_dup.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "fs/fstypes.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(fcntl_dupfd);

// High enough that no earlier test holds a handle there
static constexpr uint64_t UNUSED_HANDLE_RANGE_START = 100;

static bool has_cloexec(sched::task* task, resource::handle_t h) {
    uint32_t flags = 0;
    resource::get_handle_flags(task->handles, h, &flags);
    return (flags & resource::RESOURCE_HANDLE_CLOEXEC) != 0;
}

TEST(fcntl_dupfd, takes_the_lowest_free_handle_at_or_above_the_minimum) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/dupfd_lowest", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);

    int64_t first = 0;
    int64_t second = 0;
    RUN_ELEVATED({
        first = syscall::duplicate_handle(task, h, UNUSED_HANDLE_RANGE_START, false);
        second = syscall::duplicate_handle(task, h, UNUSED_HANDLE_RANGE_START, false);
    });
    EXPECT_EQ(first, static_cast<int64_t>(UNUSED_HANDLE_RANGE_START));
    EXPECT_EQ(second, static_cast<int64_t>(UNUSED_HANDLE_RANGE_START + 1));

    resource::close(task, static_cast<resource::handle_t>(first));
    resource::close(task, static_cast<resource::handle_t>(second));
    resource::close(task, h);
}

TEST(fcntl_dupfd, sets_close_on_exec_only_when_asked) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/dupfd_cloexec", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);
    ASSERT_EQ(resource::set_handle_flags(task->handles, h, resource::RESOURCE_HANDLE_CLOEXEC),
              resource::HANDLE_OK);

    int64_t plain = 0;
    int64_t cloexec = 0;
    RUN_ELEVATED({
        plain = syscall::duplicate_handle(task, h, 0, false);
        cloexec = syscall::duplicate_handle(task, h, 0, true);
    });
    ASSERT_TRUE(plain >= 0);
    ASSERT_TRUE(cloexec >= 0);
    EXPECT_FALSE(has_cloexec(task, static_cast<resource::handle_t>(plain)));
    EXPECT_TRUE(has_cloexec(task, static_cast<resource::handle_t>(cloexec)));
    EXPECT_TRUE(has_cloexec(task, h));

    resource::close(task, static_cast<resource::handle_t>(plain));
    resource::close(task, static_cast<resource::handle_t>(cloexec));
    resource::close(task, h);
}

TEST(fcntl_dupfd, rejects_a_bad_handle_and_a_minimum_past_the_limit) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/dupfd_limit", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);

    int64_t past_limit = 0;
    int64_t bad_handle = 0;
    RUN_ELEVATED({
        past_limit = syscall::duplicate_handle(task, h, resource::handle_limit(task), false);
        bad_handle = syscall::duplicate_handle(task, -1, 0, false);
    });
    EXPECT_EQ(past_limit, syscall::EINVAL);
    EXPECT_EQ(bad_handle, syscall::EBADF);

    resource::close(task, h);
}

TEST(fcntl_dupfd, the_copy_shares_the_open_file) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h = -1;
    uint32_t flags = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
    ASSERT_EQ(resource::open(task, "/dupfd_shared", flags, &h), resource::OK);

    int64_t copy = 0;
    RUN_ELEVATED({
        copy = syscall::duplicate_handle(task, h, 0, false);
    });
    ASSERT_TRUE(copy >= 0);

    ASSERT_EQ(resource::write(task, h, "ab", 2), static_cast<ssize_t>(2));
    ASSERT_EQ(resource::write(task, static_cast<resource::handle_t>(copy), "cd", 2),
              static_cast<ssize_t>(2));

    char buf[8] = {};
    EXPECT_EQ(resource::read_at(task, h, buf, sizeof(buf), 0), static_cast<ssize_t>(4));
    EXPECT_STREQ(buf, "abcd");

    resource::close(task, static_cast<resource::handle_t>(copy));
    resource::close(task, h);
}
