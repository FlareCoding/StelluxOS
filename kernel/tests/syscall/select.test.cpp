#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_select.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "pipe/pipe.h"
#include "common/ring_buffer.h"
#include "sync/poll.h"
#include "sync/wait_queue.h"
#include "sync/atomic.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
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
constexpr size_t   WORD_BITS   = 64;
constexpr size_t   TIMEVAL_AT  = FDSET_BYTES;
constexpr size_t   PIPE_FDS_AT = TIMEVAL_AT + 64;
constexpr size_t   SIGSET_AT   = PIPE_FDS_AT + 64;
constexpr size_t   SIGMASK_AT  = SIGSET_AT + 64;
constexpr size_t   TIMESPEC_AT = SIGMASK_AT + 64;
constexpr uint64_t SIGSET_SIZE = sizeof(signals::sig_set_t);

struct select_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct select_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct pselect_sigmask {
    uint64_t set;
    uint64_t size;
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
        set[h / WORD_BITS] |= 1ULL << (h % WORD_BITS);
        *run.page->at<select_timeval>(TIMEVAL_AT) = {0, static_cast<int64_t>(WAIT_NS / 1000)};

        uint64_t before = clock::now_ns();
        {
            user_space_scope scope(run.page->ctx);
            run.result = sys_select(static_cast<uint64_t>(h + 1), run.page->addr, 0, 0,
                                    run.page->addr + TIMEVAL_AT, 0);
        }
        run.elapsed_ns = clock::now_ns() - before;
        run.readable = (set[h / WORD_BITS] >> (h % WORD_BITS)) & 1;
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

// A probe whose queue outlives it, so an early close is counted rather than crashing
static sync::wait_queue       g_probe_queue;
static sync::atomic<uint32_t> g_probe_readable;
static sync::atomic<uint32_t> g_probe_closes;

static uint32_t probe_poll(resource::resource_object*, sync::poll_table* pt) {
    if (pt) {
        sync::poll_subscribe(*pt, g_probe_queue);
    }

    return g_probe_readable.load_acquire() ? sync::POLL_IN : 0;
}

static void probe_close(resource::resource_object*) {
    g_probe_closes.fetch_add_relaxed(1);
}

static const resource::resource_ops g_probe_ops = {
    .close = probe_close,
    .poll = probe_poll,
};

// A select on the probe with no timeout, through a handle the test closes under it
struct probe_select_run {
    user_page*                 page;
    resource::resource_object* probe;
    resource::handle_t         handle;
    sync::atomic<uint32_t>     done;
};

static probe_select_run g_probe_select;

static void run_probe_select(void* arg) {
    probe_select_run& run = *static_cast<probe_select_run*>(arg);
    sched::task* self = sched::current();

    if (resource::alloc_handle(self->handles, run.probe, resource::resource_type::PIPE, resource::RIGHT_READ,
                               &run.handle) == resource::HANDLE_OK) {
        uint64_t* set = run.page->at<uint64_t>(0);
        for (size_t i = 0; i < FDSET_BYTES / sizeof(uint64_t); i++) {
            set[i] = 0;
        }

        set[run.handle / WORD_BITS] |= 1ULL << (run.handle % WORD_BITS);

        user_space_scope scope(run.page->ctx);
        (void)sys_select(static_cast<uint64_t>(run.handle + 1), run.page->addr, 0, 0, 0, 0);
    }

    run.done.store_release(1);
    sched::exit(0);
}

TEST(select_syscall, a_select_keeps_its_objects_until_it_stops_watching_them) {
    g_probe_queue.init();
    g_probe_readable.store_relaxed(0);
    g_probe_closes.store_relaxed(0);

    user_page page;
    ASSERT_TRUE(page.ready());

    resource::resource_object* probe = heap::kalloc_new<resource::resource_object>();
    ASSERT_NOT_NULL(probe);

    probe->type = resource::resource_type::PIPE;
    probe->ops = &g_probe_ops;

    g_probe_select.page = &page;
    g_probe_select.probe = probe;
    g_probe_select.done.store_relaxed(0);

    sched::task* t = test_helpers::start_pinned_task(run_probe_select, &g_probe_select, "probe_select");
    ASSERT_NOT_NULL(t);

    EXPECT_TRUE(test_helpers::blocks_before_deadline(t));

    // Drop every other reference while the select waits
    (void)resource::close(t, g_probe_select.handle);
    resource::resource_release(probe);

    EXPECT_EQ(g_probe_closes.load_acquire(), 0u);

    g_probe_readable.store_release(1);
    sync::wake_all(g_probe_queue);

    EXPECT_TRUE(spin_wait(g_probe_select.done));
    unpin(t);

    EXPECT_EQ(g_probe_closes.load_acquire(), 1u);
}

TEST(select_syscall, pselect6_blocks_its_signal_mask_until_the_syscall_returns) {
    user_page page;
    ASSERT_TRUE(page.ready());

    sched::task* self = sched::current();
    signals::sig_set_t before = self->sig.blocked.load_acquire();
    *page.at<signals::sig_set_t>(SIGSET_AT) = signals::sig_bit(signals::SIGUSR1);
    *page.at<pselect_sigmask>(SIGMASK_AT) = {page.addr + SIGSET_AT, SIGSET_SIZE};
    *page.at<select_timespec>(TIMESPEC_AT) = {0, 0};

    int64_t selected = 0;
    {
        user_space_scope scope(page.ctx);
        selected = sys_pselect6(0, 0, 0, 0, page.addr + TIMESPEC_AT, page.addr + SIGMASK_AT);
    }

    // A direct call skips the syscall return, so the test restores the mask itself
    signals::sig_set_t during = self->sig.blocked.load_acquire();
    signals::end_temporary_blocked(self);

    EXPECT_EQ(selected, 0);
    EXPECT_EQ(during, signals::sig_bit(signals::SIGUSR1));
    EXPECT_EQ(self->sig.blocked.load_acquire(), before);
}

TEST(select_syscall, pselect6_refuses_a_malformed_signal_mask) {
    user_page page;
    ASSERT_TRUE(page.ready());

    *page.at<pselect_sigmask>(SIGMASK_AT) = {page.addr + SIGSET_AT, SIGSET_SIZE / 2};
    *page.at<select_timespec>(TIMESPEC_AT) = {0, 0};
    uintptr_t torn = page.addr + pmm::PAGE_SIZE - sizeof(pselect_sigmask) / 2;

    int64_t wrong_size = 0;
    int64_t unmapped = 0;
    {
        user_space_scope scope(page.ctx);
        wrong_size = sys_pselect6(0, 0, 0, 0, page.addr + TIMESPEC_AT, page.addr + SIGMASK_AT);
        unmapped = sys_pselect6(0, 0, 0, 0, page.addr + TIMESPEC_AT, torn);
    }

    EXPECT_EQ(wrong_size, syscall::EINVAL);
    EXPECT_EQ(unmapped, syscall::EFAULT);
    EXPECT_FALSE(sched::current()->sig.restore_mask);
}
