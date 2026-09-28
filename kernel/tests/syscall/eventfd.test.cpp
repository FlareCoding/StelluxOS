#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_eventfd.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_poll.h"
#include "eventfd/eventfd.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "fs/fstypes.h"
#include "sync/poll.h"
#include "sync/atomic.h"
#include "sched/sched.h"
#include "sched/task.h"

using test_helpers::spin_wait;
using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(eventfd_syscall);

constexpr uint64_t EFD_SEMAPHORE     = 0x1;
constexpr uint64_t UNDEFINED_FLAG    = 0x2;
constexpr int64_t  COUNT_SIZE        = sizeof(uint64_t);
constexpr uint64_t RESERVED_COUNT    = 0xFFFFFFFFFFFFFFFFULL;
constexpr size_t   COUNT_AT          = 0;
constexpr size_t   WAITER_COUNT_AT   = 16;
constexpr size_t   POLL_FD_AT        = 64;
constexpr size_t   WAITER_POLL_FD_AT = 72;
constexpr size_t   TIMEOUT_AT        = 128;

struct user_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct user_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

// A call that waits in a task of its own, since the test body must never block
enum class waiting_call { read, write, poll };

struct waiting_run {
    user_page*                 page;
    resource::resource_object* eventfd;
    waiting_call               call;
    int64_t                    result;
    sync::atomic<uint32_t>     done;
};

static int64_t create_eventfd(uint64_t initial_value, uint64_t flags) {
    return sys_eventfd2(initial_value, flags, 0, 0, 0, 0);
}

static int32_t close_handle(sched::task* task, int64_t fd) {
    return resource::close(task, static_cast<resource::handle_t>(fd));
}

// The object behind `fd`, holding a reference the caller releases
static resource::resource_object* object_of(sched::task* task, int64_t fd) {
    resource::resource_object* obj = nullptr;
    resource::handle_t handle = static_cast<resource::handle_t>(fd);
    if (resource::get_handle_object(task->handles, handle, 0, &obj) != resource::HANDLE_OK) {
        return nullptr;
    }

    return obj;
}

// Reads through the page the way userland does, leaving the count at COUNT_AT
static int64_t read_count(user_page& page, int64_t fd, uint64_t len = sizeof(uint64_t)) {
    user_space_scope scope(page.ctx);
    return sys_read(static_cast<uint64_t>(fd), page.addr + COUNT_AT, len, 0, 0, 0);
}

static int64_t write_count(user_page& page, int64_t fd, uint64_t value, uint64_t len = sizeof(uint64_t)) {
    *page.at<uint64_t>(COUNT_AT) = value;

    user_space_scope scope(page.ctx);
    return sys_write(static_cast<uint64_t>(fd), page.addr + COUNT_AT, len, 0, 0, 0);
}

// The events ppoll reports for `fd` without waiting
static int16_t ready_events(user_page& page, int64_t fd) {
    int16_t wanted = static_cast<int16_t>(sync::POLL_IN | sync::POLL_OUT);
    *page.at<user_pollfd>(POLL_FD_AT) = {static_cast<int32_t>(fd), wanted, 0};
    *page.at<user_timespec>(TIMEOUT_AT) = {0, 0};

    {
        user_space_scope scope(page.ctx);
        if (sys_ppoll(page.addr + POLL_FD_AT, 1, page.addr + TIMEOUT_AT, 0, 0, 0) < 0) {
            return -1;
        }
    }

    return page.at<user_pollfd>(POLL_FD_AT)->revents;
}

TEST(eventfd_syscall, unknown_flags_are_refused) {
    EXPECT_EQ(create_eventfd(0, fs::O_APPEND), syscall::EINVAL);
    EXPECT_EQ(create_eventfd(0, UNDEFINED_FLAG), syscall::EINVAL);
}

TEST(eventfd_syscall, nonblock_lands_on_the_object_and_cloexec_on_the_handle) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t fd = create_eventfd(0, fs::O_NONBLOCK | fs::O_CLOEXEC);
    ASSERT_TRUE(fd >= 0);

    resource::resource_object* obj = object_of(task, fd);
    ASSERT_NOT_NULL(obj);

    EXPECT_EQ(resource::get_status_flags(obj), fs::O_NONBLOCK);

    uint32_t handle_flags = 0;
    EXPECT_EQ(resource::get_handle_flags(task->handles, static_cast<resource::handle_t>(fd), &handle_flags),
              resource::HANDLE_OK);
    EXPECT_EQ(handle_flags, resource::RESOURCE_HANDLE_CLOEXEC);

    resource::resource_release(obj);
    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

TEST(eventfd_syscall, a_read_takes_the_whole_count) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(5, fs::O_NONBLOCK);
    ASSERT_TRUE(fd >= 0);

    EXPECT_EQ(write_count(page, fd, 3), COUNT_SIZE);
    EXPECT_EQ(read_count(page, fd), COUNT_SIZE);
    EXPECT_EQ(*page.at<uint64_t>(COUNT_AT), 8ULL);

    EXPECT_EQ(read_count(page, fd), syscall::EAGAIN);

    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

TEST(eventfd_syscall, a_semaphore_read_takes_one_unit) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(2, EFD_SEMAPHORE | fs::O_NONBLOCK);
    ASSERT_TRUE(fd >= 0);

    EXPECT_EQ(read_count(page, fd), COUNT_SIZE);
    EXPECT_EQ(*page.at<uint64_t>(COUNT_AT), 1ULL);

    EXPECT_EQ(read_count(page, fd), COUNT_SIZE);
    EXPECT_EQ(*page.at<uint64_t>(COUNT_AT), 1ULL);

    EXPECT_EQ(read_count(page, fd), syscall::EAGAIN);

    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

TEST(eventfd_syscall, a_transfer_moves_exactly_one_count) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(0, fs::O_NONBLOCK);
    ASSERT_TRUE(fd >= 0);

    EXPECT_EQ(read_count(page, fd, COUNT_SIZE / 2), syscall::EINVAL);
    EXPECT_EQ(write_count(page, fd, 1, COUNT_SIZE / 2), syscall::EINVAL);

    EXPECT_EQ(write_count(page, fd, RESERVED_COUNT), syscall::EINVAL);

    // A longer buffer carries only its first count
    EXPECT_EQ(write_count(page, fd, 4, 2 * COUNT_SIZE), COUNT_SIZE);
    EXPECT_EQ(read_count(page, fd, 2 * COUNT_SIZE), COUNT_SIZE);
    EXPECT_EQ(*page.at<uint64_t>(COUNT_AT), 4ULL);

    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

TEST(eventfd_syscall, a_write_past_the_maximum_would_block) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(0, fs::O_NONBLOCK);
    ASSERT_TRUE(fd >= 0);

    EXPECT_EQ(write_count(page, fd, eventfd::COUNTER_MAX), COUNT_SIZE);
    EXPECT_EQ(write_count(page, fd, 1), syscall::EAGAIN);

    EXPECT_EQ(read_count(page, fd), COUNT_SIZE);
    EXPECT_EQ(*page.at<uint64_t>(COUNT_AT), eventfd::COUNTER_MAX);

    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

TEST(eventfd_syscall, poll_reports_the_count) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(0, fs::O_NONBLOCK);
    ASSERT_TRUE(fd >= 0);

    EXPECT_EQ(ready_events(page, fd), static_cast<int16_t>(sync::POLL_OUT));

    EXPECT_EQ(write_count(page, fd, 1), COUNT_SIZE);
    EXPECT_EQ(ready_events(page, fd), static_cast<int16_t>(sync::POLL_IN | sync::POLL_OUT));

    EXPECT_EQ(write_count(page, fd, eventfd::COUNTER_MAX - 1), COUNT_SIZE);
    EXPECT_EQ(ready_events(page, fd), static_cast<int16_t>(sync::POLL_IN));

    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

static waiting_run g_waiting;

static int64_t issue_waiting_call(const waiting_run& run, resource::handle_t h) {
    uint64_t fd = static_cast<uint64_t>(h);
    uintptr_t addr = run.page->addr;

    if (run.call == waiting_call::read) {
        return sys_read(fd, addr + WAITER_COUNT_AT, sizeof(uint64_t), 0, 0, 0);
    }

    if (run.call == waiting_call::write) {
        return sys_write(fd, addr + WAITER_COUNT_AT, sizeof(uint64_t), 0, 0, 0);
    }

    return sys_ppoll(addr + WAITER_POLL_FD_AT, 1, 0, 0, 0, 0);
}

static void run_waiting_call(void* arg) {
    waiting_run& run = *static_cast<waiting_run*>(arg);
    sched::task* self = sched::current();
    resource::handle_t h = -1;
    run.result = syscall::EBADF;

    if (resource::alloc_handle(self->handles, run.eventfd, resource::resource_type::EVENTFD,
                               resource::RIGHT_READ | resource::RIGHT_WRITE, &h) == resource::HANDLE_OK) {
        *run.page->at<user_pollfd>(WAITER_POLL_FD_AT) = {h, static_cast<int16_t>(sync::POLL_IN), 0};

        {
            user_space_scope scope(run.page->ctx);
            run.result = issue_waiting_call(run, h);
        }

        (void)resource::close(self, h);
    }

    run.done.store_release(1);
    sched::exit(0);
}

static sched::task* start_waiting_call(user_page& page, resource::resource_object* eventfd, waiting_call call) {
    g_waiting.page = &page;
    g_waiting.eventfd = eventfd;
    g_waiting.call = call;
    g_waiting.result = 0;
    g_waiting.done.store_relaxed(0);

    return test_helpers::start_pinned_task(run_waiting_call, &g_waiting, "eventfd_wait");
}

TEST(eventfd_syscall, a_write_wakes_a_waiting_read) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(0, 0);
    ASSERT_TRUE(fd >= 0);

    resource::resource_object* eventfd = object_of(task, fd);
    ASSERT_NOT_NULL(eventfd);

    sched::task* t = start_waiting_call(page, eventfd, waiting_call::read);
    ASSERT_NOT_NULL(t);
    EXPECT_TRUE(test_helpers::blocks_before_deadline(t));

    EXPECT_EQ(write_count(page, fd, 9), COUNT_SIZE);
    EXPECT_TRUE(spin_wait(g_waiting.done));

    EXPECT_EQ(g_waiting.result, COUNT_SIZE);
    EXPECT_EQ(*page.at<uint64_t>(WAITER_COUNT_AT), 9ULL);

    unpin(t);
    resource::resource_release(eventfd);
    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

TEST(eventfd_syscall, a_read_wakes_a_waiting_write) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(0, 0);
    ASSERT_TRUE(fd >= 0);

    resource::resource_object* eventfd = object_of(task, fd);
    ASSERT_NOT_NULL(eventfd);

    EXPECT_EQ(write_count(page, fd, eventfd::COUNTER_MAX), COUNT_SIZE);
    *page.at<uint64_t>(WAITER_COUNT_AT) = 1;

    sched::task* t = start_waiting_call(page, eventfd, waiting_call::write);
    ASSERT_NOT_NULL(t);
    EXPECT_TRUE(test_helpers::blocks_before_deadline(t));

    EXPECT_EQ(read_count(page, fd), COUNT_SIZE);
    EXPECT_TRUE(spin_wait(g_waiting.done));

    // The waiting write lands once the read makes room
    EXPECT_EQ(g_waiting.result, COUNT_SIZE);
    EXPECT_EQ(read_count(page, fd), COUNT_SIZE);
    EXPECT_EQ(*page.at<uint64_t>(COUNT_AT), 1ULL);

    unpin(t);
    resource::resource_release(eventfd);
    EXPECT_EQ(close_handle(task, fd), resource::OK);
}

TEST(eventfd_syscall, a_write_wakes_a_waiting_poll) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t fd = create_eventfd(0, 0);
    ASSERT_TRUE(fd >= 0);

    resource::resource_object* eventfd = object_of(task, fd);
    ASSERT_NOT_NULL(eventfd);

    sched::task* t = start_waiting_call(page, eventfd, waiting_call::poll);
    ASSERT_NOT_NULL(t);
    EXPECT_TRUE(test_helpers::blocks_before_deadline(t));

    EXPECT_EQ(write_count(page, fd, 1), COUNT_SIZE);
    EXPECT_TRUE(spin_wait(g_waiting.done));

    EXPECT_EQ(g_waiting.result, 1);
    EXPECT_EQ(page.at<user_pollfd>(WAITER_POLL_FD_AT)->revents, static_cast<int16_t>(sync::POLL_IN));

    unpin(t);
    resource::resource_release(eventfd);
    EXPECT_EQ(close_handle(task, fd), resource::OK);
}
