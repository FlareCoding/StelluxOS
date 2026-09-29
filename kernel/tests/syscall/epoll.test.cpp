#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_epoll.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/epoll_event.h"
#include "epoll/epoll.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "fs/fstypes.h"
#include "signals/signal.h"
#include "sync/poll.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/pmm.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(epoll_syscall);

constexpr uint64_t EPOLL_CLOEXEC  = fs::O_CLOEXEC;
constexpr uint64_t UNDEFINED_FLAG = 0x1;
constexpr uint64_t EPOLL_CTL_ADD  = 1;
constexpr uint64_t EPOLL_CTL_DEL  = 2;
constexpr uint64_t EPOLL_CTL_MOD  = 3;
constexpr uint64_t UNDEFINED_OP   = 4;
constexpr uint32_t EPOLLONESHOT   = 1u << 30;
constexpr int64_t  MISSING_FD     = 999;
constexpr uint64_t READ_DATA      = 0xC0FFEE;
constexpr uint64_t WRITE_DATA     = 0xBEEF;
constexpr uint64_t WAIT_EVENTS    = 4;
constexpr uint64_t SIGSET_SIZE    = sizeof(signals::sig_set_t);
constexpr size_t   PIPE_FDS_AT    = 0;
constexpr size_t   BYTE_AT        = 16;
constexpr size_t   EVENT_AT       = 32;
constexpr size_t   SIGSET_AT      = 64;
constexpr size_t   EVENTS_AT      = 128;

// An epoll and a pipe holding one byte, so both of the pipe's ends are ready
struct watched_pipe {
    sched::task* task;
    user_page    page;
    int64_t      epfd     = -1;
    int32_t      read_fd  = -1;
    int32_t      write_fd = -1;

    watched_pipe() : task(sched::current()) {
        if (!page.ready()) {
            return;
        }

        user_space_scope scope(page.ctx);
        epfd = sys_epoll_create1(0, 0, 0, 0, 0, 0);
        if (epfd < 0 || sys_pipe2(page.addr + PIPE_FDS_AT, 0, 0, 0, 0, 0) != 0) {
            return;
        }

        read_fd = page.at<int32_t>(PIPE_FDS_AT)[0];
        write_fd = page.at<int32_t>(PIPE_FDS_AT)[1];
        *page.at<uint8_t>(BYTE_AT) = 1;
        (void)sys_write(static_cast<uint64_t>(write_fd), page.addr + BYTE_AT, 1, 0, 0, 0);
    }

    ~watched_pipe() {
        if (read_fd >= 0) {
            (void)resource::close(task, read_fd);
            (void)resource::close(task, write_fd);
        }

        if (epfd >= 0) {
            (void)resource::close(task, static_cast<resource::handle_t>(epfd));
        }
    }

    bool ready() const { return write_fd >= 0; }

    int64_t ctl(uint64_t op, int64_t fd, uint32_t events, uint64_t data, int64_t on_epfd = -1) {
        *page.at<syscall::epoll_event>(EVENT_AT) = {events, data};

        user_space_scope scope(page.ctx);
        return sys_epoll_ctl(on_epfd >= 0 ? on_epfd : epfd, op, fd, page.addr + EVENT_AT, 0, 0);
    }

    int64_t wait(uint64_t max_events, uint64_t events_at = 0) {
        user_space_scope scope(page.ctx);
        return sys_epoll_pwait(epfd, events_at ? events_at : page.addr + EVENTS_AT, max_events, 0, 0, 0);
    }

    // Copied out of the packed record, whose fields the test macros cannot bind references to
    epoll::ready_event reported(size_t i) {
        syscall::epoll_event record = page.at<syscall::epoll_event>(EVENTS_AT)[i];
        return {record.events, record.data};
    }
};

TEST(epoll_syscall, unknown_flags_and_sizes_are_refused) {
    EXPECT_EQ(sys_epoll_create1(UNDEFINED_FLAG, 0, 0, 0, 0, 0), syscall::EINVAL);
    EXPECT_EQ(sys_epoll_create(0, 0, 0, 0, 0, 0), syscall::EINVAL);
}

TEST(epoll_syscall, cloexec_lands_on_the_handle) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t closing = sys_epoll_create1(EPOLL_CLOEXEC, 0, 0, 0, 0, 0);
    int64_t plain = sys_epoll_create(1, 0, 0, 0, 0, 0);
    ASSERT_TRUE(closing >= 0);
    ASSERT_TRUE(plain >= 0);

    uint32_t closing_flags = 0;
    uint32_t plain_flags = 0;
    EXPECT_EQ(resource::get_handle_flags(task->handles, static_cast<resource::handle_t>(closing), &closing_flags),
              resource::HANDLE_OK);
    EXPECT_EQ(resource::get_handle_flags(task->handles, static_cast<resource::handle_t>(plain), &plain_flags),
              resource::HANDLE_OK);
    EXPECT_EQ(closing_flags, resource::RESOURCE_HANDLE_CLOEXEC);
    EXPECT_EQ(plain_flags, 0u);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(closing)), resource::OK);
    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(plain)), resource::OK);
}

TEST(epoll_syscall, an_interest_is_added_changed_and_removed) {
    watched_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.read_fd, sync::POLL_IN, READ_DATA), 0);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.read_fd, sync::POLL_IN, READ_DATA), syscall::EEXIST);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_MOD, pipe.read_fd, sync::POLL_IN | sync::POLL_RDHUP, WRITE_DATA), 0);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_DEL, pipe.read_fd, 0, 0), 0);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_DEL, pipe.read_fd, 0, 0), syscall::ENOENT);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_MOD, pipe.read_fd, sync::POLL_IN, READ_DATA), syscall::ENOENT);
}

TEST(epoll_syscall, bad_handles_ops_modes_and_targets_are_refused) {
    watched_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    resource::handle_t directory = -1;
    ASSERT_EQ(resource::open(pipe.task, "/", fs::O_RDONLY, &directory), resource::OK);

    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.read_fd, sync::POLL_IN, READ_DATA, MISSING_FD), syscall::EBADF);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, MISSING_FD, sync::POLL_IN, READ_DATA), syscall::EBADF);
    EXPECT_EQ(pipe.ctl(UNDEFINED_OP, pipe.read_fd, sync::POLL_IN, READ_DATA), syscall::EINVAL);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.epfd, sync::POLL_IN, READ_DATA), syscall::EINVAL);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.write_fd, sync::POLL_OUT, WRITE_DATA, pipe.read_fd), syscall::EINVAL);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.read_fd, sync::POLL_IN | EPOLLONESHOT, READ_DATA), syscall::EINVAL);
    EXPECT_EQ(pipe.ctl(EPOLL_CTL_ADD, directory, sync::POLL_IN, READ_DATA), syscall::EPERM);

    // A removal never reads the event, so an unmapped one only faults for an add
    uintptr_t torn = pipe.page.addr + pmm::PAGE_SIZE - sizeof(syscall::epoll_event) / 2;
    int64_t added = 0;
    int64_t removed = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        added = sys_epoll_ctl(pipe.epfd, EPOLL_CTL_ADD, pipe.read_fd, torn, 0, 0);
        removed = sys_epoll_ctl(pipe.epfd, EPOLL_CTL_DEL, pipe.read_fd, torn, 0, 0);
    }

    EXPECT_EQ(added, syscall::EFAULT);
    EXPECT_EQ(removed, syscall::ENOENT);

    EXPECT_EQ(resource::close(pipe.task, directory), resource::OK);
}

TEST(epoll_syscall, ready_ends_are_reported_in_the_abi_layout) {
    watched_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    ASSERT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.read_fd, sync::POLL_IN, READ_DATA), 0);
    ASSERT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.write_fd, sync::POLL_OUT, WRITE_DATA), 0);

    ASSERT_EQ(pipe.wait(WAIT_EVENTS), 2);
    EXPECT_EQ(pipe.reported(0).events, sync::POLL_IN);
    EXPECT_EQ(pipe.reported(0).data, READ_DATA);
    EXPECT_EQ(pipe.reported(1).events, sync::POLL_OUT);
    EXPECT_EQ(pipe.reported(1).data, WRITE_DATA);

    int64_t waited = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        waited = sys_epoll_wait(pipe.epfd, pipe.page.addr + EVENTS_AT, 1, 0, 0, 0);
    }

    EXPECT_EQ(waited, 1);
}

TEST(epoll_syscall, a_wait_refuses_bad_counts_and_handles_and_faults_on_an_unmapped_buffer) {
    watched_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    EXPECT_EQ(pipe.wait(WAIT_EVENTS), 0);
    EXPECT_EQ(pipe.wait(0), syscall::EINVAL);
    EXPECT_EQ(pipe.wait(static_cast<uint64_t>(-1)), syscall::EINVAL);

    int64_t missing = 0;
    int64_t not_epoll = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        missing = sys_epoll_pwait(MISSING_FD, pipe.page.addr + EVENTS_AT, WAIT_EVENTS, 0, 0, 0);
        not_epoll = sys_epoll_pwait(pipe.read_fd, pipe.page.addr + EVENTS_AT, WAIT_EVENTS, 0, 0, 0);
    }

    EXPECT_EQ(missing, syscall::EBADF);
    EXPECT_EQ(not_epoll, syscall::EINVAL);

    ASSERT_EQ(pipe.ctl(EPOLL_CTL_ADD, pipe.read_fd, sync::POLL_IN, READ_DATA), 0);
    EXPECT_EQ(pipe.wait(WAIT_EVENTS, pipe.page.addr + pmm::PAGE_SIZE - sizeof(syscall::epoll_event) / 2),
              syscall::EFAULT);
}

TEST(epoll_syscall, epoll_pwait_blocks_its_signal_mask_until_the_syscall_returns) {
    watched_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    signals::sig_set_t before = pipe.task->sig.blocked.load_acquire();
    *pipe.page.at<signals::sig_set_t>(SIGSET_AT) = signals::sig_bit(signals::SIGUSR1);

    int64_t waited = 0;
    int64_t wrong_size = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        wrong_size = sys_epoll_pwait(pipe.epfd, pipe.page.addr + EVENTS_AT, WAIT_EVENTS, 0, pipe.page.addr + SIGSET_AT,
                                     SIGSET_SIZE / 2);
        waited = sys_epoll_pwait(pipe.epfd, pipe.page.addr + EVENTS_AT, WAIT_EVENTS, 0, pipe.page.addr + SIGSET_AT,
                                 SIGSET_SIZE);
    }

    // A direct call skips the syscall return, so the test restores the mask itself
    signals::sig_set_t during = pipe.task->sig.blocked.load_acquire();
    signals::end_temporary_blocked(pipe.task);

    EXPECT_EQ(wrong_size, syscall::EINVAL);
    EXPECT_EQ(waited, 0);
    EXPECT_EQ(during, signals::sig_bit(signals::SIGUSR1));
    EXPECT_EQ(pipe.task->sig.blocked.load_acquire(), before);
}
