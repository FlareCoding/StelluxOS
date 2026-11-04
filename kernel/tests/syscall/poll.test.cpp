#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_poll.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sync/poll.h"
#include "sync/atomic.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "mm/pmm.h"

using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(poll_syscall);

constexpr int64_t  NS_PER_SEC        = 1000000000LL;
constexpr size_t   PIPE_FDS_AT       = 0;
constexpr size_t   POLL_FD_AT        = 64;
constexpr size_t   TIMEOUT_AT        = 128;
constexpr size_t   SIGSET_AT         = 192;
constexpr size_t   WAITER_POLL_FD_AT = 256;
constexpr size_t   WAITER_TIMEOUT_AT = 320;
constexpr size_t   BYTE_AT           = 384;
constexpr uint64_t SIGSET_SIZE       = sizeof(signals::sig_set_t);

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

// A ppoll on a pipe's read end, issued from a task of its own since the test body must never block
struct read_end_ppoll {
    user_page*                 page;
    resource::resource_object* reader;
    int64_t                    result;
    sync::atomic<uint32_t>     done;
};

// A pipe polled on its write end, which is always ready, since the test body must never block
struct polled_pipe {
    sched::task*               task;
    user_page                  page;
    int32_t                    read_fd  = -1;
    int32_t                    write_fd = -1;
    resource::resource_object* reader   = nullptr;

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
        (void)resource::get_handle_object(task->handles, read_fd, resource::RIGHT_READ, &reader);
        *page.at<user_pollfd>(POLL_FD_AT) = {write_fd, static_cast<int16_t>(sync::POLL_OUT), 0};
    }

    ~polled_pipe() {
        if (reader) {
            resource::resource_release(reader);
        }

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

// UINT64_MAX ns, the longest timeout whose count of nanoseconds fits in 64 bits
constexpr user_timespec LONGEST_TIMEOUT = {
    static_cast<int64_t>(~0ULL / NS_PER_SEC),
    static_cast<int64_t>(~0ULL % NS_PER_SEC)
};

static read_end_ppoll g_longest_ppoll;

static void ppoll_read_end_with_uint64_max_ns(void* arg) {
    read_end_ppoll& run = *static_cast<read_end_ppoll*>(arg);
    sched::task* self = sched::current();
    resource::handle_t h = -1;
    run.result = syscall::EBADF;

    if (resource::alloc_handle(self->handles, run.reader, resource::resource_type::PIPE, resource::RIGHT_READ,
                               &h) == resource::HANDLE_OK) {
        *run.page->at<user_pollfd>(WAITER_POLL_FD_AT) = {h, static_cast<int16_t>(sync::POLL_IN), 0};
        *run.page->at<user_timespec>(WAITER_TIMEOUT_AT) = LONGEST_TIMEOUT;

        {
            user_space_scope scope(run.page->ctx);
            run.result = sys_ppoll(run.page->addr + WAITER_POLL_FD_AT, 1, run.page->addr + WAITER_TIMEOUT_AT, 0, 0, 0);
        }

        (void)resource::close(self, h);
    }

    run.done.store_release(1);
    sched::exit(0);
}

// A wait with a timeout whose deadline would pass UINT64_MAX neither returns at once nor spins
TEST(poll_syscall, ppoll_with_a_timeout_of_uint64_max_ns_sleeps_until_a_descriptor_is_ready) {
    polled_pipe pipe;
    ASSERT_TRUE(pipe.ready());
    ASSERT_NOT_NULL(pipe.reader);

    g_longest_ppoll.page = &pipe.page;
    g_longest_ppoll.reader = pipe.reader;
    g_longest_ppoll.result = 0;
    g_longest_ppoll.done.store_relaxed(0);

    sched::task* t = test_helpers::start_pinned_task_on_another_cpu(
        ppoll_read_end_with_uint64_max_ns,
        &g_longest_ppoll,
        "ppoll_longest",
        sched::TASK_FLAG_ELEVATED
    );
    ASSERT_NOT_NULL(t);
    bool slept = test_helpers::stays_asleep(t);

    *pipe.page.at<uint8_t>(BYTE_AT) = 1;
    int64_t written = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        written = sys_write(static_cast<uint64_t>(pipe.write_fd), pipe.page.addr + BYTE_AT, 1, 0, 0, 0);
    }

    EXPECT_TRUE(test_helpers::finish_or_kill(t, g_longest_ppoll.done));
    unpin(t);

    EXPECT_TRUE(slept);
    EXPECT_EQ(written, 1);
    EXPECT_EQ(g_longest_ppoll.result, 1);
    EXPECT_EQ(pipe.page.at<user_pollfd>(WAITER_POLL_FD_AT)->revents, static_cast<int16_t>(sync::POLL_IN));
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

TEST(poll_syscall, ppoll_blocks_its_signal_mask_until_the_syscall_returns) {
    polled_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    signals::sig_set_t before = pipe.task->sig.blocked.load_acquire();
    *pipe.page.at<signals::sig_set_t>(SIGSET_AT) = signals::sig_bit(signals::SIGUSR1);
    *pipe.page.at<user_timespec>(TIMEOUT_AT) = {0, 0};

    int64_t ready = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        ready = sys_ppoll(pipe.page.addr + POLL_FD_AT, 1, pipe.page.addr + TIMEOUT_AT, pipe.page.addr + SIGSET_AT,
                          SIGSET_SIZE, 0);
    }

    // A direct call skips the syscall return, so the test restores the mask itself
    signals::sig_set_t during = pipe.task->sig.blocked.load_acquire();
    signals::end_temporary_blocked(pipe.task);

    EXPECT_EQ(ready, 1);
    EXPECT_EQ(during, signals::sig_bit(signals::SIGUSR1));
    EXPECT_EQ(pipe.task->sig.blocked.load_acquire(), before);
}

TEST(poll_syscall, ppoll_refuses_a_malformed_signal_mask) {
    polled_pipe pipe;
    ASSERT_TRUE(pipe.ready());

    *pipe.page.at<user_timespec>(TIMEOUT_AT) = {0, 0};
    uintptr_t torn = pipe.page.addr + pmm::PAGE_SIZE - SIGSET_SIZE / 2;

    int64_t wrong_size = 0;
    int64_t unmapped = 0;
    {
        user_space_scope scope(pipe.page.ctx);
        wrong_size = sys_ppoll(pipe.page.addr + POLL_FD_AT, 1, pipe.page.addr + TIMEOUT_AT,
                               pipe.page.addr + SIGSET_AT, SIGSET_SIZE / 2, 0);
        unmapped = sys_ppoll(pipe.page.addr + POLL_FD_AT, 1, pipe.page.addr + TIMEOUT_AT, torn, SIGSET_SIZE, 0);
    }

    EXPECT_EQ(wrong_size, syscall::EINVAL);
    EXPECT_EQ(unmapped, syscall::EFAULT);
    EXPECT_FALSE(pipe.task->sig.restore_mask);
}
