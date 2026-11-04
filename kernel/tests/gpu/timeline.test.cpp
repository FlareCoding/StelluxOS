#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fake_backend.h"
#include "clock/clock.h"
#include "fs/fs.h"

TEST_SUITE(gpu_timeline);

using gpu_test::close_handle;
using gpu_test::session;
using gpu_test::write_wait;
using test_helpers::spin_wait;
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::stays_asleep;
using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

constexpr uint64_t NO_DEADLINE = ~static_cast<uint64_t>(0);
constexpr uint64_t PASSED_DEADLINE = 0;
constexpr uint64_t SHORT_WAIT_NS = 100 * 1000 * 1000;
constexpr uint32_t WAIT_ANY = 0;
constexpr uint32_t UNKNOWN_WAIT_FLAG = 0x2u;

// A wait issued from a task of its own, through its own file, page and handles, since the test body must never block.
// Static per test, since a task that never finishes keeps using it
struct gpu_waiter {
    const char*                path;
    resource::resource_object* timeline_objects[2];
    uint64_t                   targets[2];
    uint32_t                   count;
    uint32_t                   flags;
    uint64_t                   deadline_ns;
    resource::handle_t         handles[2];
    sched::task*               task;
    int32_t                    result;
    sync::atomic<uint32_t>     done;
};

static void run_wait_on_its_task(void* arg) {
    gpu_waiter& run = *static_cast<gpu_waiter*>(arg);
    sched::task* task = sched::current();
    {
        user_page page;
        fs::file* file = fs::open(run.path, fs::O_RDWR);

        gpu_point points[2] = {};
        for (uint32_t i = 0; i < run.count; i++) {
            resource::resource_object* obj = run.timeline_objects[i];
            resource::alloc_task_handle(task, obj, obj->type, resource::RIGHT_READ, &run.handles[i]);
            points[i] = gpu_test::point(run.handles[i], run.targets[i]);
        }

        run.result = fs::ERR_NOMEM;
        if (file && page.ready()) {
            write_wait(page, points, run.count, run.flags, run.deadline_ns);
            user_space_scope scope(page.ctx);
            run.result = fs::ioctl(file, GPU_TIMELINE_WAIT, page.addr + gpu_test::ARGS_AT);
        }

        for (uint32_t i = 0; i < run.count; i++) {
            resource::close(task, run.handles[i]);
        }

        if (file) {
            fs::close(file);
        }
    }

    run.done.store_release(1);
    sched::exit(0);
}

// Starts `run` waiting for each of `handles` to reach its target, and with no deadline waits until it blocks
static bool start_wait(
    session& s,
    gpu_waiter& run,
    const int32_t* handles,
    const uint64_t* targets,
    uint32_t count,
    uint32_t flags,
    uint64_t deadline
) {
    run.path = s.path;
    run.count = count;
    run.flags = flags;
    run.deadline_ns = deadline;
    run.done.store_relaxed(0);

    resource::handle_table* table = sched::current()->handles;
    for (uint32_t i = 0; i < count; i++) {
        resource::handle_t h = static_cast<resource::handle_t>(handles[i]);
        if (resource::get_handle_object(table, h, 0, &run.timeline_objects[i]) != resource::HANDLE_OK) {
            return false;
        }

        resource::resource_release(run.timeline_objects[i]);
        run.targets[i] = targets[i];
    }

    run.task = start_pinned_task_on_another_cpu(run_wait_on_its_task, &run, "gpu_wait", sched::TASK_FLAG_ELEVATED);
    return run.task && (deadline != NO_DEADLINE || test_helpers::blocks_before_deadline(run.task));
}

// Waits for the task to finish, false when it has not by the spin deadline
static bool finish_wait(gpu_waiter& run) {
    bool done = spin_wait(run.done);
    unpin(run.task);
    return done;
}

static gpu_test::fake_backend g_signal_fake;

// A timeline starts at its creator's value and only rises
TEST(gpu_timeline, signal_raises_the_value_and_refuses_one_at_or_below_it) {
    session s(g_signal_fake);
    ASSERT_TRUE(s.ready());

    int32_t timeline = s.new_timeline(5);
    ASSERT_TRUE(timeline >= 0);
    EXPECT_EQ(s.query(timeline), static_cast<uint64_t>(5));

    EXPECT_EQ(s.signal(timeline, 5), syscall::EINVAL);
    EXPECT_EQ(s.signal(timeline, 4), syscall::EINVAL);
    EXPECT_EQ(s.query(timeline), static_cast<uint64_t>(5));

    EXPECT_EQ(s.signal(timeline, 9), static_cast<int64_t>(0));
    EXPECT_EQ(s.query(timeline), static_cast<uint64_t>(9));

    close_handle(timeline);
}

static gpu_test::fake_backend g_rights_fake;

// Signal needs RIGHT_WRITE, query and wait need RIGHT_READ, and a buffer's handle names no timeline
TEST(gpu_timeline, each_call_needs_its_right_on_a_timeline_handle) {
    session s(g_rights_fake);
    ASSERT_TRUE(s.ready());

    int32_t timeline = s.new_timeline(1);
    ASSERT_TRUE(timeline >= 0);
    int32_t writer = s.restrict_to(timeline, GPU_RIGHT_WRITE);
    ASSERT_TRUE(writer >= 0);
    int32_t reader = s.restrict_to(timeline, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0);

    int32_t buffer = s.new_buffer(pmm::PAGE_SIZE, GPU_PLACEMENT_VIDEO, 0);
    ASSERT_TRUE(buffer >= 0);

    EXPECT_EQ(s.signal(writer, 2), static_cast<int64_t>(0));
    EXPECT_EQ(s.signal(reader, 3), syscall::EBADF);
    EXPECT_EQ(s.query(timeline), static_cast<uint64_t>(2));
    EXPECT_EQ(s.query(writer), gpu_test::QUERY_FAILED);
    EXPECT_EQ(s.signal(buffer, 3), syscall::EBADF);
    EXPECT_EQ(s.query(buffer), gpu_test::QUERY_FAILED);

    gpu_point through_writer = gpu_test::point(writer, 1);
    write_wait(s.page, &through_writer, 1, WAIT_ANY, PASSED_DEADLINE);
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::EBADF);

    gpu_point through_buffer = gpu_test::point(buffer, 1);
    write_wait(s.page, &through_buffer, 1, WAIT_ANY, PASSED_DEADLINE);
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::EBADF);

    close_handle(buffer);
    close_handle(reader);
    close_handle(writer);
    close_handle(timeline);
}

static gpu_test::fake_backend g_arguments_fake;

// Create and signal refuse nonzero flags, which a query ignores and writes out as zero, and a wait refuses a count of 0
// or past GPU_POINTS_MAX, an unknown flag and nonzero padding in its argument or a point, yet takes GPU_POINTS_MAX
TEST(gpu_timeline, each_call_refuses_each_invalid_argument) {
    session s(g_arguments_fake);
    ASSERT_TRUE(s.ready());

    gpu_timeline_args created = gpu_test::timeline_args(-1, 0);
    created.flags = 1;
    EXPECT_EQ(s.call(GPU_TIMELINE_CREATE, created), syscall::EINVAL);
    EXPECT_EQ(created.timeline, -1);

    int32_t timeline = s.new_timeline(1);
    ASSERT_TRUE(timeline >= 0);
    gpu_timeline_args signalled = gpu_test::timeline_args(timeline, 2);
    signalled.flags = 1;
    EXPECT_EQ(s.call(GPU_TIMELINE_SIGNAL, signalled), syscall::EINVAL);

    gpu_timeline_args queried = gpu_test::timeline_args(timeline, 0);
    queried.flags = 1;
    EXPECT_EQ(s.call(GPU_TIMELINE_QUERY, queried), static_cast<int64_t>(0));
    EXPECT_EQ(queried.flags, 0u);
    EXPECT_EQ(queried.value, static_cast<uint64_t>(1));

    gpu_point point = gpu_test::point(timeline, 1);
    write_wait(s.page, &point, 1, WAIT_ANY, PASSED_DEADLINE)->count = 0;
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::EINVAL);
    write_wait(s.page, &point, 1, WAIT_ANY, PASSED_DEADLINE)->count = GPU_POINTS_MAX + 1;
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::EINVAL);

    write_wait(s.page, &point, 1, WAIT_ANY, PASSED_DEADLINE)->flags = UNKNOWN_WAIT_FLAG;
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::EINVAL);
    write_wait(s.page, &point, 1, WAIT_ANY, PASSED_DEADLINE)->padding = 1;
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::EINVAL);

    gpu_point padded = point;
    padded.padding = 1;
    write_wait(s.page, &padded, 1, WAIT_ANY, PASSED_DEADLINE);
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::EINVAL);

    gpu_point most[GPU_POINTS_MAX] = {};
    for (gpu_point& each : most) {
        each = point;
    }

    write_wait(s.page, most, GPU_POINTS_MAX, GPU_WAIT_ALL, PASSED_DEADLINE);
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), static_cast<int64_t>(0));

    close_handle(timeline);
}

static gpu_test::fake_backend g_any_fake;

// A wait for any point returns at once when one is reached, even past its deadline, and reports which
TEST(gpu_timeline, a_wait_for_any_reports_the_reached_point) {
    session s(g_any_fake);
    ASSERT_TRUE(s.ready());

    int32_t behind = s.new_timeline(0);
    int32_t ahead = s.new_timeline(7);
    ASSERT_TRUE(behind >= 0);
    ASSERT_TRUE(ahead >= 0);

    gpu_point points[3] = {gpu_test::point(ahead, 7), gpu_test::point(behind, 1), gpu_test::point(behind, 2)};
    gpu_wait_args* args = write_wait(s.page, points, 3, WAIT_ANY, PASSED_DEADLINE);
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), static_cast<int64_t>(0));
    EXPECT_EQ(args->reached, 0u);

    points[0] = gpu_test::point(behind, 1);
    points[2] = gpu_test::point(ahead, 6);
    args = write_wait(s.page, points, 3, WAIT_ANY, PASSED_DEADLINE);
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), static_cast<int64_t>(0));
    EXPECT_EQ(args->reached, 2u);

    close_handle(ahead);
    close_handle(behind);
}

static gpu_test::fake_backend g_all_fake;
static gpu_waiter g_all_wait = {};

// A wait for all points sleeps through the first rise and returns at the second
TEST(gpu_timeline, a_wait_for_all_sleeps_until_every_point_is_reached) {
    session s(g_all_fake);
    ASSERT_TRUE(s.ready());

    int32_t timelines[2] = {s.new_timeline(0), s.new_timeline(0)};
    ASSERT_TRUE(timelines[0] >= 0);
    ASSERT_TRUE(timelines[1] >= 0);

    uint64_t targets[2] = {1, 2};
    ASSERT_TRUE(start_wait(s, g_all_wait, timelines, targets, 2, GPU_WAIT_ALL, NO_DEADLINE));

    EXPECT_EQ(s.signal(timelines[0], 1), static_cast<int64_t>(0));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(g_all_wait.task));
    EXPECT_EQ(g_all_wait.done.load_acquire(), 0u);

    EXPECT_EQ(s.signal(timelines[1], 2), static_cast<int64_t>(0));
    EXPECT_TRUE(finish_wait(g_all_wait));
    EXPECT_EQ(g_all_wait.result, fs::OK);

    close_handle(timelines[1]);
    close_handle(timelines[0]);
}

static gpu_test::fake_backend g_deadline_fake;
static gpu_waiter g_deadline_wait = {};

// A sleeping wait ends at its deadline, and a wait whose deadline has passed returns ETIMEDOUT through the syscall. The
// sleeping wait runs first, so one that never ends fails the test before the test task makes a wait of its own
TEST(gpu_timeline, a_wait_ends_with_etimedout_at_its_deadline) {
    session s(g_deadline_fake);
    ASSERT_TRUE(s.ready());

    int32_t timeline = s.new_timeline(0);
    ASSERT_TRUE(timeline >= 0);

    uint64_t target = 1;
    uint64_t deadline = clock::now_ns() + SHORT_WAIT_NS;
    ASSERT_TRUE(start_wait(s, g_deadline_wait, &timeline, &target, 1, WAIT_ANY, deadline));
    ASSERT_TRUE(finish_wait(g_deadline_wait));
    ASSERT_EQ(g_deadline_wait.result, fs::ERR_TIMEDOUT);
    EXPECT_GE(clock::now_ns(), deadline);

    gpu_point point = gpu_test::point(timeline, 1);
    write_wait(s.page, &point, 1, GPU_WAIT_ALL, PASSED_DEADLINE);
    EXPECT_EQ(s.call_in_place(GPU_TIMELINE_WAIT), syscall::ETIMEDOUT);

    close_handle(timeline);
}

static gpu_test::fake_backend g_latest_fake;
static gpu_waiter g_latest_wait = {};

// A wait whose deadline is one below UINT64_MAX sleeps until its point is reached instead of spinning
TEST(gpu_timeline, a_wait_with_the_latest_deadline_sleeps_until_its_point_is_reached) {
    session s(g_latest_fake);
    ASSERT_TRUE(s.ready());

    int32_t timeline = s.new_timeline(0);
    ASSERT_TRUE(timeline >= 0);

    uint64_t target = 1;
    ASSERT_TRUE(start_wait(s, g_latest_wait, &timeline, &target, 1, WAIT_ANY, NO_DEADLINE - 1));
    bool slept = stays_asleep(g_latest_wait.task);

    EXPECT_EQ(s.signal(timeline, 1), static_cast<int64_t>(0));
    EXPECT_TRUE(finish_wait(g_latest_wait));
    EXPECT_EQ(g_latest_wait.result, fs::OK);
    EXPECT_TRUE(slept);

    close_handle(timeline);
}

static gpu_test::fake_backend g_held_fake;
static gpu_waiter g_held_wait = {};

// A wait holds each timeline it names, so closing every handle to one while the wait sleeps leaves its point unreached
TEST(gpu_timeline, a_wait_holds_each_timeline_it_names_until_it_returns) {
    session s(g_held_fake);
    ASSERT_TRUE(s.ready());

    int32_t timelines[2] = {s.new_timeline(0), s.new_timeline(0)};
    ASSERT_TRUE(timelines[0] >= 0);
    ASSERT_TRUE(timelines[1] >= 0);

    uint64_t targets[2] = {1, 1};
    ASSERT_TRUE(start_wait(s, g_held_wait, timelines, targets, 2, GPU_WAIT_ALL, NO_DEADLINE));

    resource::resource_object* first = nullptr;
    resource::handle_t first_handle = static_cast<resource::handle_t>(timelines[0]);
    ASSERT_EQ(resource::get_handle_object(sched::current()->handles, first_handle, 0, &first), resource::HANDLE_OK);

    close_handle(timelines[0]);
    EXPECT_EQ(resource::close(g_held_wait.task, g_held_wait.handles[0]), resource::OK);
    EXPECT_EQ(first->ref_count(), 2u);

    EXPECT_EQ(s.signal(timelines[1], 1), static_cast<int64_t>(0));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(g_held_wait.task));

    sched::force_wake_for_kill(g_held_wait.task);
    EXPECT_TRUE(finish_wait(g_held_wait));
    EXPECT_EQ(g_held_wait.result, fs::ERR_INTR);
    ASSERT_EQ(first->ref_count(), 1u);

    resource::resource_release(first);
    close_handle(timelines[1]);
}

static gpu_test::fake_backend g_interrupted_fake;
static gpu_waiter g_interrupted_wait = {};

// Killing the waiting task ends its wait with ERR_INTR
TEST(gpu_timeline, a_kill_interrupts_a_sleeping_wait) {
    session s(g_interrupted_fake);
    ASSERT_TRUE(s.ready());

    int32_t timeline = s.new_timeline(0);
    ASSERT_TRUE(timeline >= 0);

    uint64_t target = 1;
    ASSERT_TRUE(start_wait(s, g_interrupted_wait, &timeline, &target, 1, WAIT_ANY, NO_DEADLINE));

    sched::force_wake_for_kill(g_interrupted_wait.task);
    EXPECT_TRUE(finish_wait(g_interrupted_wait));
    EXPECT_EQ(g_interrupted_wait.result, fs::ERR_INTR);

    close_handle(timeline);
}
