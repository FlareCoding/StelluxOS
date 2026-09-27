#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_select.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "pipe/pipe.h"
#include "common/ring_buffer.h"
#include "sync/wait_queue.h"
#include "sync/atomic.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "clock/clock.h"

using test_helpers::spin_wait;
using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(select_syscall);

constexpr uint64_t MS          = 1000000ULL;
constexpr uint64_t WAIT_NS     = 200 * MS;
constexpr uint32_t STRAY_WAKES = 3;
constexpr size_t   FDSET_BYTES = 128;
constexpr size_t   TIMEVAL_AT  = FDSET_BYTES;
constexpr size_t   PIPE_FDS_AT = TIMEVAL_AT + 64;

struct select_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

// A select run in a task of its own, since the test body runs on CPU 0's
// idle task, which must never block
struct select_run {
    user_page*                 page;
    resource::resource_object* reader;
    int64_t                    result;
    uint64_t                   elapsed_ns;
    bool                       readable;
    sync::atomic<uint32_t>     done;
};

// The pipe the select watches, its ring buffer woken directly with nothing to read
struct selected_pipe {
    sched::task*               task;
    user_page                  page;
    int32_t                    read_fd  = -1;
    int32_t                    write_fd = -1;
    resource::resource_object* reader   = nullptr;
    ring_buffer*               rb       = nullptr;

    selected_pipe() : task(sched::current()) {
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
        if (resource::get_handle_object(task->handles, read_fd, resource::RIGHT_READ, &reader) ==
            resource::HANDLE_OK) {
            rb = static_cast<pipe::pipe_endpoint*>(reader->impl)->channel->rb;
        }
    }

    ~selected_pipe() {
        if (reader) {
            resource::resource_release(reader);
        }
        if (read_fd >= 0) {
            (void)resource::close(task, read_fd);
            (void)resource::close(task, write_fd);
        }
    }

    bool ready() const { return rb != nullptr; }
};

static select_run g_select;

static void run_select(void* arg) {
    select_run& run = *static_cast<select_run*>(arg);
    sched::task* self = sched::current();
    resource::handle_t h = -1;
    run.result = syscall::EBADF;

    if (resource::alloc_handle(self->handles, run.reader, resource::resource_type::PIPE, resource::RIGHT_READ,
                               &h) == resource::HANDLE_OK) {
        uint64_t* set = run.page->at<uint64_t>(0);
        for (size_t i = 0; i < FDSET_BYTES / sizeof(uint64_t); i++) {
            set[i] = 0;
        }
        set[h / 64] |= 1ULL << (h % 64);
        *run.page->at<select_timeval>(TIMEVAL_AT) = {0, static_cast<int64_t>(WAIT_NS / 1000)};

        uint64_t before = clock::now_ns();
        {
            user_space_scope scope(run.page->ctx);
            run.result = sys_select(static_cast<uint64_t>(h + 1), run.page->addr, 0, 0,
                                    run.page->addr + TIMEVAL_AT, 0);
        }
        run.elapsed_ns = clock::now_ns() - before;
        run.readable = (set[h / 64] >> (h % 64)) & 1;
        (void)resource::close(self, h);
    }

    run.done.store_release(1);
    sched::exit(0);
}

static sched::task* start_select(selected_pipe& pipe) {
    g_select.page = &pipe.page;
    g_select.reader = pipe.reader;
    g_select.result = 0;
    g_select.elapsed_ns = 0;
    g_select.readable = false;
    g_select.done.store_relaxed(0);
    return test_helpers::start_pinned_task(run_select, &g_select, "select_run");
}

// True once the selecting task has blocked again, false when it finished first
static bool select_blocks_again(sched::task* t) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (clock::now_ns() < deadline && !g_select.done.load_acquire()) {
        if (t->state.load_acquire() == sched::TASK_STATE_BLOCKED) {
            return true;
        }
    }

    return false;
}

static uint32_t deliver_stray_wakes(sched::task* t, ring_buffer* rb) {
    uint32_t delivered = 0;
    while (delivered < STRAY_WAKES && select_blocks_again(t)) {
        sync::wake_all(rb->read_wq);
        delivered++;
    }

    return delivered;
}

TEST(select_syscall, a_wake_that_leaves_nothing_ready_is_not_a_timeout) {
    selected_pipe pipe;
    ASSERT_TRUE(pipe.ready());
    sched::task* t = start_select(pipe);
    ASSERT_NOT_NULL(t);

    EXPECT_EQ(deliver_stray_wakes(t, pipe.rb), STRAY_WAKES);
    EXPECT_TRUE(spin_wait(g_select.done));
    unpin(t);

    EXPECT_EQ(g_select.result, 0);
    EXPECT_GE(g_select.elapsed_ns, WAIT_NS);
}

TEST(select_syscall, data_after_stray_wakes_still_ends_the_wait_early) {
    selected_pipe pipe;
    ASSERT_TRUE(pipe.ready());
    sched::task* t = start_select(pipe);
    ASSERT_NOT_NULL(t);

    EXPECT_EQ(deliver_stray_wakes(t, pipe.rb), STRAY_WAKES);
    EXPECT_TRUE(select_blocks_again(t));
    uint8_t byte = 1;
    (void)ring_buffer_write(pipe.rb, &byte, 1, true);
    EXPECT_TRUE(spin_wait(g_select.done));
    unpin(t);

    EXPECT_EQ(g_select.result, 1);
    EXPECT_LT(g_select.elapsed_ns, WAIT_NS);
    EXPECT_TRUE(g_select.readable);
}
