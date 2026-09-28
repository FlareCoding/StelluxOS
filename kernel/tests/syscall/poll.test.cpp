#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_poll.h"
#include "resource/resource.h"
#include "sync/poll.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/pmm.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(poll_syscall);

constexpr int64_t NS_PER_SEC  = 1000000000LL;
constexpr size_t  PIPE_FDS_AT = 0;
constexpr size_t  POLL_FD_AT  = 64;
constexpr size_t  TIMEOUT_AT  = 128;

// Long enough that each round subscribes, although the polled end is always ready
constexpr uint64_t WAIT_MS = 1000;

struct user_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct user_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

// A pipe polled on its write end, which is always ready, since the test body must never block
struct polled_pipe {
    sched::task* task;
    user_page    page;
    int32_t      read_fd  = -1;
    int32_t      write_fd = -1;

    polled_pipe() : task(sched::current()) {
        if (!page.ready()) {
            return;
        }

        {
            user_space_scope scope(page.ctx);
            if (sys_pipe2(page.addr + PIPE_FDS_AT, 0, 0, 0, 0, 0) != 0) {
                return;
            }
        }

        read_fd = page.at<int32_t>(PIPE_FDS_AT)[0];
        write_fd = page.at<int32_t>(PIPE_FDS_AT)[1];
        *page.at<user_pollfd>(POLL_FD_AT) = {write_fd, static_cast<int16_t>(sync::POLL_OUT), 0};
    }

    ~polled_pipe() {
        if (read_fd >= 0) {
            (void)resource::close(task, read_fd);
            (void)resource::close(task, write_fd);
        }
    }

    bool ready() const { return write_fd >= 0; }

    int16_t revents() { return page.at<user_pollfd>(POLL_FD_AT)->revents; }
};

static int64_t ppoll_write_end(polled_pipe& pipe, int64_t nsec) {
    *pipe.page.at<user_timespec>(TIMEOUT_AT) = {0, nsec};

    user_space_scope scope(pipe.page.ctx);
    return sys_ppoll(pipe.page.addr + POLL_FD_AT, 1, pipe.page.addr + TIMEOUT_AT, 0, 0, 0);
}

TEST(poll_syscall, ppoll_reports_a_ready_descriptor) {
    polled_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    EXPECT_EQ(ppoll_write_end(pipe, NS_PER_SEC - 1), 1);
    EXPECT_EQ(pipe.revents(), static_cast<int16_t>(sync::POLL_OUT));
}

TEST(poll_syscall, ppoll_refuses_out_of_range_nanoseconds) {
    polled_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    EXPECT_EQ(ppoll_write_end(pipe, NS_PER_SEC), syscall::EINVAL);
}

TEST(poll_syscall, poll_reports_a_ready_descriptor) {
    polled_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    int64_t ready = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        ready = sys_poll(pipe.page.addr + POLL_FD_AT, 1, WAIT_MS, 0, 0, 0);
    }

    EXPECT_EQ(ready, 1);
    EXPECT_EQ(pipe.revents(), static_cast<int16_t>(sync::POLL_OUT));
}

TEST(poll_syscall, a_descriptor_array_past_mapped_memory_faults) {
    user_page page;
    ASSERT_TRUE(page.ready());

    // The entry's second half lies on the unmapped page after the user page
    uintptr_t torn = page.addr + pmm::PAGE_SIZE - sizeof(user_pollfd) / 2;

    int64_t polled = 0;
    int64_t ppolled = 0;
    {
        user_space_scope scope(page.ctx);
        polled = sys_poll(torn, 1, 0, 0, 0, 0);
        ppolled = sys_ppoll(torn, 1, 0, 0, 0, 0);
    }

    EXPECT_EQ(polled, syscall::EFAULT);
    EXPECT_EQ(ppolled, syscall::EFAULT);
}
