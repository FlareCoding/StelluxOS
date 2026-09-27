#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_select.h"
#include "resource/resource.h"
#include "pipe/pipe.h"
#include "common/ring_buffer.h"
#include "sync/wait_queue.h"
#include "sync/atomic.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(select_syscall);

constexpr uint64_t MS          = 1000000ULL;
constexpr uint64_t WAIT_NS     = 200 * MS;
constexpr uint32_t STRAY_WAKES = 3;
constexpr size_t   FDSET_BYTES = 128;
constexpr size_t   TIMEVAL_AT  = FDSET_BYTES;

struct select_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

// The pipe under test: the read end selected on, its ring buffer woken from aside
struct selected_pipe {
    sched::task*  task;
    user_page     page;
    int32_t       read_fd  = -1;
    int32_t       write_fd = -1;
    ring_buffer*  rb       = nullptr;

    selected_pipe() : task(sched::current()) {
        if (!page.ready()) {
            return;
        }

        user_space_scope scope(page.ctx);
        if (sys_pipe2(page.addr, 0, 0, 0, 0, 0) != 0) {
            return;
        }

        read_fd = page.at<int32_t>(0)[0];
        write_fd = page.at<int32_t>(0)[1];

        resource::resource_object* obj = nullptr;
        if (resource::get_handle_object(task->handles, read_fd, 0, &obj) == resource::HANDLE_OK) {
            rb = static_cast<pipe::pipe_endpoint*>(obj->impl)->channel->rb;
            resource::resource_release(obj);
        }
    }

    ~selected_pipe() {
        if (read_fd >= 0) {
            (void)resource::close(task, read_fd);
            (void)resource::close(task, write_fd);
        }
    }

    bool ready() const { return rb != nullptr; }

    // select on the read end alone, with WAIT_NS to wait
    int64_t select_read() {
        uint64_t* set = page.at<uint64_t>(0);
        for (size_t i = 0; i < FDSET_BYTES / sizeof(uint64_t); i++) {
            set[i] = 0;
        }
        set[read_fd / 64] |= 1ULL << (read_fd % 64);
        *page.at<select_timeval>(TIMEVAL_AT) = {0, static_cast<int64_t>(WAIT_NS / 1000)};

        user_space_scope scope(page.ctx);
        return sys_select(static_cast<uint64_t>(read_fd + 1), page.addr, 0, 0, page.addr + TIMEVAL_AT, 0);
    }
};

static sched::task*          g_select_waiter;
static ring_buffer*          g_select_rb;
static sync::atomic<uint32_t> g_select_write_after;
static sync::atomic<uint32_t> g_select_helper_done;

static bool wait_until_blocked(sched::task* t) {
    uint64_t give_up = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (t->state.load_acquire() != sched::TASK_STATE_BLOCKED) {
        if (clock::now_ns() > give_up) return false;
    }

    return true;
}

// Wakes the subscribed queue a few times with nothing to read, then writes a
// byte when asked to
static void select_disturber_fn(void*) {
    for (uint32_t i = 0; i < STRAY_WAKES; i++) {
        bool blocked = false;
        RUN_ELEVATED(blocked = wait_until_blocked(g_select_waiter));
        if (!blocked) {
            break;
        }

        RUN_ELEVATED(sync::wake_all(g_select_rb->read_wq));
        uint64_t pause = clock::now_ns() + 10 * MS;
        while (clock::now_ns() < pause) {
            cpu::relax();
        }
    }

    if (g_select_write_after.load_acquire()) {
        uint8_t byte = 1;
        RUN_ELEVATED((void)ring_buffer_write(g_select_rb, &byte, 1, true));
    }

    g_select_helper_done.store_release(1);
    sched::exit(0);
}

static void start_disturber(selected_pipe& pipe, bool write_after) {
    g_select_waiter = pipe.task;
    g_select_rb = pipe.rb;
    g_select_write_after.store_relaxed(write_after ? 1 : 0);
    g_select_helper_done.store_relaxed(0);
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(select_disturber_fn, nullptr, "select_disturber");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });
}

TEST(select_syscall, a_wake_that_leaves_nothing_ready_is_not_a_timeout) {
    selected_pipe pipe;
    ASSERT_TRUE(pipe.ready());
    start_disturber(pipe, false);

    uint64_t before = clock::now_ns();
    int64_t ready = pipe.select_read();
    uint64_t elapsed = clock::now_ns() - before;

    EXPECT_EQ(ready, 0);
    EXPECT_GE(elapsed, WAIT_NS);
    ASSERT_TRUE(test_helpers::spin_wait(g_select_helper_done));
}

TEST(select_syscall, data_after_stray_wakes_still_ends_the_wait_early) {
    selected_pipe pipe;
    ASSERT_TRUE(pipe.ready());
    start_disturber(pipe, true);

    uint64_t before = clock::now_ns();
    int64_t ready = pipe.select_read();
    uint64_t elapsed = clock::now_ns() - before;

    EXPECT_EQ(ready, 1);
    EXPECT_LT(elapsed, WAIT_NS);
    EXPECT_TRUE(pipe.page.at<uint64_t>(0)[pipe.read_fd / 64] & (1ULL << (pipe.read_fd % 64)));
    ASSERT_TRUE(test_helpers::spin_wait(g_select_helper_done));
}
