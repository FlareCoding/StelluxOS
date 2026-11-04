#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fake_backend.h"
#include "clock/clock.h"
#include "fs/fs.h"

TEST_SUITE(gpu_submission);

using gpu_test::close_handle;
using gpu_test::FAKE_VA_START;
using gpu_test::fake_queue;
using gpu_test::point;
using gpu_test::session;
using test_helpers::spin_wait_ge;

constexpr uint64_t PAGE = pmm::PAGE_SIZE;
constexpr uint32_t SMALL = 16;
constexpr uint32_t LARGEST_LIST = static_cast<uint32_t>(gpu_test::COMMANDS_ROOM);
constexpr uint64_t SHORT_TIMEOUT_NS = 50ull * 1000 * 1000;

// One test's context with one graphics queue, index 0, and a timeline at 0, all closed with the setup
struct queue_setup {
    session&            s;
    gpu_test::fake_backend& fake;
    int32_t             context = -1;
    int32_t             timeline = -1;
    fake_queue*         queue = nullptr;

    queue_setup(session& on, gpu_test::fake_backend& backend) : s(on), fake(backend) {
        context = s.new_context();
        if (context >= 0 && s.new_queue(context, GPU_QUEUE_GRAPHICS) == 0) {
            queue = fake.queue_at(fake.queue_count - 1);
            timeline = s.new_timeline(0);
        }
    }

    ~queue_setup() {
        if (timeline >= 0) {
            close_handle(timeline);
        }

        if (context >= 0) {
            close_handle(context);
        }
    }

    bool ready() const { return queue && timeline >= 0; }

    int64_t submit(uint8_t tag, const gpu_point* waits, uint32_t wait_count, const gpu_point* signals,
                   uint32_t signal_count) {
        return s.submit(context, 0, tag, SMALL, waits, wait_count, signals, signal_count);
    }
};

// The timeline's value once it rose past `below`, or `below` when it has not by the spin deadline
static uint64_t value_past(session& s, int32_t timeline, uint64_t below) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    uint64_t value = s.query(timeline);
    while (value == below && clock::now_ns() < deadline) {
        value = s.query(timeline);
    }

    return value;
}

// The timeline's `GPU_TIMELINE_*` flags, or `QUERY_FAILED` when the query fails
static uint64_t flags_of(session& s, int32_t timeline) {
    gpu_timeline_args args = gpu_test::timeline_args(timeline, 0);
    return s.call(GPU_TIMELINE_QUERY, args) == 0 ? args.flags : gpu_test::QUERY_FAILED;
}

// Spins for `ns`, so a deadline that passes within it can fire while the test still watches
static void spin_for(uint64_t ns) {
    uint64_t until = clock::now_ns() + ns;
    while (clock::now_ns() < until) {
        test_helpers::brief_delay();
    }
}

// Spins until the queue's deadline timer left its tree, which it does as its callback starts, and returns whether it
// did by the spin deadline
static bool deadline_fired(const fake_queue& queue) {
    uint64_t until = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (timer::is_pending(&queue.core->deadline) && clock::now_ns() < until) {
        test_helpers::brief_delay();
    }

    return !timer::is_pending(&queue.core->deadline);
}

// Maps `size` zeroed bytes in the session's address space for a command list, returning their address or 0
static uintptr_t map_list(session& s, size_t size) {
    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    uint32_t flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
    if (mm::mm_context_map_anonymous(s.page.ctx, 0, size, prot, flags, &addr) != mm::MM_CTX_OK) {
        return 0;
    }

    return addr;
}

static gpu_test::fake_backend g_order_fake;

// Each submission runs at the queue's next sequence number, and progress, clamped to the last run and ignored at or
// below the last reported, reaches the signals of every submission it covers and restarts the deadline of the rest
TEST(gpu_submission, submissions_run_in_order_and_progress_reaches_their_signals) {
    session s(g_order_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_order_fake);
    ASSERT_TRUE(q.ready());

    gpu_point first = point(q.timeline, 1);
    gpu_point second = point(q.timeline, 2);
    EXPECT_EQ(q.submit(0xA1, nullptr, 0, &first, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.submit(q.context, 0, 0xA2, 0, nullptr, 0, &second, 1), static_cast<int64_t>(0));

    EXPECT_EQ(q.queue->runs, 2u);
    EXPECT_EQ(q.queue->last_sequence, static_cast<uint64_t>(2));
    EXPECT_EQ(q.queue->last_size, static_cast<size_t>(0));
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(0));
    uint64_t started_deadline_ns = q.queue->core->deadline_ns;

    gpu::progress(q.queue->core, 1);
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(1));
    uint64_t restarted_deadline_ns = q.queue->core->deadline_ns;
    EXPECT_TRUE(restarted_deadline_ns > started_deadline_ns);

    gpu::progress(q.queue->core, 1);
    EXPECT_EQ(q.queue->core->deadline_ns, restarted_deadline_ns);
    gpu::progress(q.queue->core, 9);
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(2));
    EXPECT_EQ(flags_of(s, q.timeline), static_cast<uint64_t>(0));

    gpu_point third = point(q.timeline, 3);
    EXPECT_EQ(q.submit(0xA3, nullptr, 0, &third, 1), static_cast<int64_t>(0));
    EXPECT_EQ(q.queue->last_sequence, static_cast<uint64_t>(3));
    gpu::progress(q.queue->core, 3);
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(3));
}

static gpu_test::fake_backend g_hold_fake;

// A submission is held until each of its waits is reached, one held keeps every later one on its queue held, and a
// wait reached after its queue ran lets that queue run again
TEST(gpu_submission, a_submission_is_held_until_its_waits_are_reached) {
    session s(g_hold_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_hold_fake);
    ASSERT_TRUE(q.ready());
    int32_t gate = s.new_timeline(0);
    ASSERT_TRUE(gate >= 0);

    gpu_point waits[2] = {point(gate, 3), point(gate, 5)};
    gpu_point later = point(gate, 7);
    gpu_point done = point(q.timeline, 1);

    EXPECT_EQ(q.submit(0xB1, waits, 2, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(q.submit(0xB2, nullptr, 0, &done, 1), static_cast<int64_t>(0));
    EXPECT_EQ(q.submit(0xB3, &later, 1, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(q.queue->runs, 0u);

    EXPECT_EQ(s.signal(gate, 4), static_cast<int64_t>(0));
    EXPECT_EQ(q.queue->runs, 0u);
    EXPECT_EQ(s.signal(gate, 5), static_cast<int64_t>(0));
    EXPECT_EQ(q.queue->runs, 2u);
    EXPECT_EQ(q.queue->last_first_byte, 0xB2);

    EXPECT_EQ(s.signal(gate, 7), static_cast<int64_t>(0));
    EXPECT_EQ(q.queue->runs, 3u);
    EXPECT_EQ(q.queue->last_first_byte, 0xB3);

    gpu::progress(q.queue->core, 2);
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(1));

    close_handle(gate);
}

static gpu_test::fake_backend g_room_fake;

// A list the running ones leave no room for stays held first on its queue, with the lists after it, until progress
// frees that room
TEST(gpu_submission, a_list_without_room_stays_held_until_progress_frees_room) {
    g_room_fake.running_bytes_max = GPU_COMMANDS_BYTES_MAX;
    session s(g_room_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_room_fake);
    ASSERT_TRUE(q.ready());

    uint32_t filling = GPU_COMMANDS_BYTES_MAX / LARGEST_LIST;
    for (uint32_t i = 0; i < filling; i++) {
        EXPECT_EQ(s.submit(q.context, 0, 0x31, LARGEST_LIST, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    }

    gpu_point done = point(q.timeline, 1);
    EXPECT_EQ(q.submit(0x32, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.submit(q.context, 0, 0x33, 0, nullptr, 0, &done, 1), static_cast<int64_t>(0));
    EXPECT_EQ(q.queue->runs, filling);

    gpu::progress(q.queue->core, 1);
    EXPECT_EQ(q.queue->runs, filling + 2);
    EXPECT_EQ(q.queue->last_size, static_cast<size_t>(0));
    gpu::progress(q.queue->core, filling + 2);
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(1));
}

static gpu_test::fake_backend g_readying_fake;

// Progress on one queue runs the work its signals readied on another, and work a destroy ended before that point was
// reached never runs
TEST(gpu_submission, progress_runs_the_work_it_readies_on_another_queue) {
    session s(g_readying_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_readying_fake);
    ASSERT_TRUE(q.ready());

    ASSERT_EQ(s.new_queue(q.context, GPU_QUEUE_COPY), static_cast<int64_t>(1));
    ASSERT_EQ(s.new_queue(q.context, GPU_QUEUE_COPY), static_cast<int64_t>(2));
    fake_queue* readied = g_readying_fake.queue_at(1);
    fake_queue* destroyed = g_readying_fake.queue_at(2);
    int32_t after = s.new_timeline(0);
    ASSERT_TRUE(after >= 0);

    gpu_point first = point(q.timeline, 1);
    gpu_point next = point(after, 1);
    EXPECT_EQ(q.submit(0x41, nullptr, 0, &first, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.submit(q.context, 1, 0x42, SMALL, &first, 1, &next, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.submit(q.context, 2, 0x43, SMALL, &first, 1, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.destroy_queue(q.context, 2), static_cast<int64_t>(0));

    gpu::progress(q.queue->core, 1);
    EXPECT_EQ(readied->runs, 1u);
    EXPECT_EQ(readied->last_first_byte, 0x42);
    EXPECT_EQ(destroyed->runs, 0u);
    EXPECT_TRUE(spin_wait_ge(g_readying_fake.queues_destroyed, 1));

    gpu::progress(readied->core, 1);
    EXPECT_EQ(s.query(after), static_cast<uint64_t>(1));
    EXPECT_EQ(flags_of(s, after), static_cast<uint64_t>(0));

    close_handle(after);
}

static gpu_test::fake_backend g_failure_fake;

// A destroyed queue ends its held work failed at once and its running work at its stop, each signal reached with the
// failed mark, and a submission waiting on a failed point ends failed without running once its other waits are reached
TEST(gpu_submission, failed_work_fails_its_signals_and_the_work_waiting_on_them) {
    session s(g_failure_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_failure_fake);
    ASSERT_TRUE(q.ready());

    ASSERT_EQ(s.new_queue(q.context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(1));
    fake_queue* second = g_failure_fake.queue_at(1);
    int32_t stopped = s.new_timeline(0);
    int32_t gate = s.new_timeline(0);
    int32_t after = s.new_timeline(0);
    ASSERT_TRUE(stopped >= 0 && gate >= 0 && after >= 0);

    gpu_point ran = point(stopped, 1);
    gpu_point waits_held = point(after, 1);
    gpu_point held = point(q.timeline, 2);
    EXPECT_EQ(q.submit(0xC1, nullptr, 0, &ran, 1), static_cast<int64_t>(0));
    EXPECT_EQ(q.submit(0xC2, &waits_held, 1, &held, 1), static_cast<int64_t>(0));

    gpu_point waits_failed[2] = {held, point(gate, 1)};
    gpu_point follow = point(after, 2);
    EXPECT_EQ(s.submit(q.context, 1, 0xC3, SMALL, waits_failed, 2, &follow, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.submit(q.context, 1, 0xC4, SMALL, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(second->runs, 0u);

    EXPECT_EQ(s.destroy_queue(q.context, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(2));
    EXPECT_EQ(flags_of(s, q.timeline), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(s.query(after), static_cast<uint64_t>(0));
    EXPECT_EQ(second->runs, 0u);

    EXPECT_EQ(s.signal(gate, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.query(after), static_cast<uint64_t>(2));
    EXPECT_EQ(flags_of(s, after), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(second->runs, 1u);
    EXPECT_EQ(second->last_first_byte, 0xC4);

    EXPECT_TRUE(spin_wait_ge(g_failure_fake.queues_destroyed, 1));
    EXPECT_EQ(s.query(stopped), static_cast<uint64_t>(1));
    EXPECT_EQ(flags_of(s, stopped), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(s.submit(q.context, 0, 0xC5, SMALL, nullptr, 0, nullptr, 0), syscall::EINVAL);

    gpu_point late_follow = point(after, 3);
    EXPECT_EQ(s.submit(q.context, 1, 0xC6, SMALL, &ran, 1, &late_follow, 1), static_cast<int64_t>(0));
    EXPECT_EQ(second->runs, 1u);
    EXPECT_EQ(s.query(after), static_cast<uint64_t>(3));

    close_handle(stopped);
    close_handle(gate);
    close_handle(after);
}

static gpu_test::fake_backend g_close_fake;

// Closing a context ends its held work failed at the first stage, so a later signal of that work's wait runs
// nothing, and its running work failed at the queue's stop. The teardown still destroys the queue and the space
TEST(gpu_submission, a_closed_context_ends_held_work_at_once_and_running_work_at_the_stop) {
    g_close_fake.stops_on_reaper = false;
    session s(g_close_fake);
    ASSERT_TRUE(s.ready());
    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));

    int32_t gate = s.new_timeline(0);
    int32_t ran = s.new_timeline(0);
    int32_t held = s.new_timeline(0);
    ASSERT_TRUE(gate >= 0 && ran >= 0 && held >= 0);

    gpu_point ran_point = point(ran, 1);
    gpu_point gate_point = point(gate, 1);
    gpu_point held_point = point(held, 1);
    EXPECT_EQ(s.submit(context, 0, 0x51, SMALL, nullptr, 0, &ran_point, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.submit(context, 0, 0x52, SMALL, &gate_point, 1, &held_point, 1), static_cast<int64_t>(0));

    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_close_fake.stops_asked, 1));
    EXPECT_EQ(s.query(held), static_cast<uint64_t>(1));
    EXPECT_EQ(flags_of(s, held), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(s.query(ran), static_cast<uint64_t>(0));

    fake_queue& queue = g_close_fake.queues[0];
    EXPECT_EQ(s.signal(gate, 1), static_cast<int64_t>(0));
    EXPECT_EQ(queue.runs, 1u);
    gpu::queue_stopped(queue.core);
    EXPECT_EQ(s.query(ran), static_cast<uint64_t>(1));
    EXPECT_EQ(flags_of(s, ran), static_cast<uint64_t>(GPU_TIMELINE_FAILED));

    EXPECT_TRUE(spin_wait_ge(g_close_fake.spaces_destroyed, 1));
    EXPECT_EQ(g_close_fake.queues_destroyed.load_acquire(), 1u);

    close_handle(gate);
    close_handle(ran);
    close_handle(held);
}

static gpu_test::fake_backend g_hung_fake;

// Running work past the backend's submit timeout loses its context as hung, which ends the work failed and fails a
// later submit, map or queue create on the context with EIO
TEST(gpu_submission, a_passed_deadline_loses_the_context_as_hung) {
    g_hung_fake.submit_timeout_ns = SHORT_TIMEOUT_NS;
    session s(g_hung_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_hung_fake);
    ASSERT_TRUE(q.ready());

    gpu_point done = point(q.timeline, 1);
    EXPECT_EQ(q.submit(0xD1, nullptr, 0, &done, 1), static_cast<int64_t>(0));
    EXPECT_TRUE(spin_wait_ge(g_hung_fake.stops_asked, 1));
    EXPECT_EQ(s.lost(q.context), static_cast<int64_t>(GPU_LOST_HUNG));
    EXPECT_EQ(q.submit(0xD2, nullptr, 0, nullptr, 0), syscall::EIO);

    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);
    EXPECT_EQ(s.map(q.context, buffer, FAKE_VA_START, 0, PAGE, 0), syscall::EIO);
    EXPECT_EQ(s.new_queue(q.context, GPU_QUEUE_GRAPHICS), syscall::EIO);

    EXPECT_EQ(value_past(s, q.timeline, 0), static_cast<uint64_t>(1));
    EXPECT_EQ(flags_of(s, q.timeline), static_cast<uint64_t>(GPU_TIMELINE_FAILED));

    close_handle(buffer);
}

static gpu_test::fake_backend g_stopping_deadline_fake;

// A deadline passing on a queue already asked to stop leaves the context live and asks no second stop, and the work
// still ends failed at the stop
TEST(gpu_submission, a_deadline_passing_on_a_stopping_queue_loses_nothing) {
    g_stopping_deadline_fake.submit_timeout_ns = SHORT_TIMEOUT_NS;
    g_stopping_deadline_fake.stops_on_reaper = false;
    session s(g_stopping_deadline_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_stopping_deadline_fake);
    ASSERT_TRUE(q.ready());

    gpu_point done = point(q.timeline, 1);
    EXPECT_EQ(q.submit(0x61, nullptr, 0, &done, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.destroy_queue(q.context, 0), static_cast<int64_t>(0));

    EXPECT_TRUE(deadline_fired(*q.queue));
    spin_for(SHORT_TIMEOUT_NS);
    EXPECT_EQ(s.lost(q.context), static_cast<int64_t>(0));
    EXPECT_EQ(g_stopping_deadline_fake.stops_asked.load_acquire(), 1u);

    gpu::queue_stopped(q.queue->core);
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(1));
    EXPECT_EQ(flags_of(s, q.timeline), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_TRUE(spin_wait_ge(g_stopping_deadline_fake.queues_destroyed, 1));
}

static gpu_test::fake_backend g_create_loss_fake;

// A context a deadline loses before `GPU_QUEUE_CREATE` publishes its queue, the fake holding `create_queue` for it,
// gets that queue asked to stop, and the create returns EIO
TEST(gpu_submission, a_context_lost_during_queue_create_stops_that_queue) {
    g_create_loss_fake.submit_timeout_ns = SHORT_TIMEOUT_NS;
    session s(g_create_loss_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_create_loss_fake);
    ASSERT_TRUE(q.ready());

    EXPECT_EQ(q.submit(0x71, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    g_create_loss_fake.create_waits_for_a_stop = true;
    EXPECT_EQ(s.new_queue(q.context, GPU_QUEUE_COPY), syscall::EIO);
    EXPECT_EQ(s.lost(q.context), static_cast<int64_t>(GPU_LOST_HUNG));
    EXPECT_EQ(g_create_loss_fake.stops_asked.load_acquire(), 2u);
    EXPECT_TRUE(g_create_loss_fake.queues[1].stop_asked);
}

static gpu_test::fake_backend g_fault_fake;

// A context whose space is lost reports the first reason, an engine error as well as a fault with its address,
// ends its held work failed at once and refuses buffers placed in it
TEST(gpu_submission, a_lost_space_reports_its_first_reason_and_address) {
    g_fault_fake.placements = gpu_test::ALL_PLACEMENTS;
    session s(g_fault_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_fault_fake);
    ASSERT_TRUE(q.ready());

    int32_t gate = s.new_timeline(0);
    ASSERT_TRUE(gate >= 0);
    gpu_point gate_point = point(gate, 1);
    gpu_point held = point(q.timeline, 1);
    EXPECT_EQ(q.submit(0xE0, &gate_point, 1, &held, 1), static_cast<int64_t>(0));

    gpu::space_lost(g_fault_fake.spaces[0].core, GPU_LOST_FAULT, FAKE_VA_START + PAGE);
    gpu::space_lost(g_fault_fake.spaces[0].core, GPU_LOST_ENGINE, 0);
    EXPECT_EQ(flags_of(s, q.timeline), static_cast<uint64_t>(GPU_TIMELINE_FAILED));

    gpu_context_args args = {.context = q.context, .lost = 0, .fault_va = 0};
    EXPECT_EQ(s.call(GPU_CONTEXT_QUERY, args), static_cast<int64_t>(0));
    EXPECT_EQ(args.lost, GPU_LOST_FAULT);
    EXPECT_EQ(args.fault_va, FAKE_VA_START + PAGE);
    EXPECT_TRUE(q.queue->stop_asked);
    EXPECT_EQ(q.submit(0xE1, nullptr, 0, nullptr, 0), syscall::EIO);

    gpu_buffer_args placed = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_CONTEXT, 0);
    placed.context = q.context;
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, placed), syscall::EIO);
    EXPECT_EQ(g_fault_fake.create_calls, 0u);

    int32_t other = s.new_context();
    ASSERT_TRUE(other >= 0);
    gpu::space_lost(g_fault_fake.spaces[1].core, GPU_LOST_ENGINE, 0);
    EXPECT_EQ(s.lost(other), static_cast<int64_t>(GPU_LOST_ENGINE));

    close_handle(other);
    close_handle(gate);
}

static gpu_test::fake_backend g_loss_window_fake;
static int64_t g_submit_in_loss = 0;

// Submits to queue 1 from the loss's stop request of queue 0, before the loss asks queue 1 to stop
static void submit_in_loss(void* arg) {
    auto& q = *static_cast<queue_setup*>(arg);
    g_submit_in_loss = q.s.submit(q.context, 1, 0x73, SMALL, nullptr, 0, nullptr, 0);
}

// A loss marks its context before it asks each queue to stop, and a submit in between returns EIO and runs nothing
TEST(gpu_submission, a_submit_while_a_loss_asks_its_queues_to_stop_returns_eio) {
    session s(g_loss_window_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_loss_window_fake);
    ASSERT_TRUE(q.ready());
    ASSERT_EQ(s.new_queue(q.context, GPU_QUEUE_COPY), static_cast<int64_t>(1));

    g_loss_window_fake.on_stop = submit_in_loss;
    g_loss_window_fake.on_stop_arg = &q;
    gpu::space_lost(g_loss_window_fake.spaces[0].core, GPU_LOST_ENGINE, 0);
    EXPECT_EQ(g_submit_in_loss, syscall::EIO);
    EXPECT_EQ(g_loss_window_fake.queues[1].runs, 0u);
    EXPECT_EQ(g_loss_window_fake.stops_asked.load_acquire(), 2u);
}

static gpu_test::fake_backend g_unmap_fake;

// An unmap while work runs keeps the buffer's memory until that work finished, even with every handle closed, and
// an unmap while no work runs releases it with the context still open
TEST(gpu_submission, unmap_keeps_the_memory_until_running_work_finished) {
    session s(g_unmap_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_unmap_fake);
    ASSERT_TRUE(q.ready());

    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);
    ASSERT_EQ(s.map(q.context, buffer, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));

    EXPECT_EQ(q.submit(0xF1, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    close_handle(buffer);
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START, PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(g_unmap_fake.released_count.load_acquire(), 0u);

    gpu::progress(q.queue->core, 1);
    EXPECT_TRUE(spin_wait_ge(g_unmap_fake.released_count, 1));

    int32_t idle = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(idle >= 0);
    ASSERT_EQ(s.map(q.context, idle, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    close_handle(idle);
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START, PAGE), static_cast<int64_t>(0));
    EXPECT_TRUE(spin_wait_ge(g_unmap_fake.released_count, 2));
}

static gpu_test::fake_backend g_markers_fake;

// Each buffer an unmap cuts keeps its memory until the work running at that unmap finished, both parts of a split
// mapping and each buffer of an unmap across two mappings included, and a mapping cut at either end keeps its buffer
TEST(gpu_submission, each_buffer_an_unmap_cuts_stays_until_the_work_then_running_finished) {
    session s(g_markers_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_markers_fake);
    ASSERT_TRUE(q.ready());

    int32_t split = s.new_buffer(3 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    int32_t whole = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    int32_t probe = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(split >= 0 && whole >= 0 && probe >= 0);
    gpu::memory* probe_memory = g_markers_fake.last_created;
    ASSERT_EQ(s.map(q.context, split, FAKE_VA_START, 0, 3 * PAGE, 0), static_cast<int64_t>(0));

    ASSERT_EQ(s.map(q.context, whole, FAKE_VA_START + 3 * PAGE, 0, PAGE, 0), static_cast<int64_t>(0));
    close_handle(split);
    close_handle(whole);
    EXPECT_EQ(q.submit(0x81, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START + PAGE, PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(q.submit(0x82, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));

    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START, 4 * PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(g_markers_fake.last_unmap.size, 4 * PAGE);
    EXPECT_EQ(g_markers_fake.spaces[0].range_count, 0u);
    EXPECT_FALSE(g_markers_fake.spaces[0].bad_call);

    // The probe's release runs on the reaper after any release deferred before its close
    gpu::progress(q.queue->core, 1);
    close_handle(probe);
    EXPECT_TRUE(spin_wait_ge(g_markers_fake.released_count, 1));
    EXPECT_TRUE(g_markers_fake.last_released == probe_memory);
    EXPECT_EQ(g_markers_fake.released_count.load_acquire(), 1u);

    gpu::progress(q.queue->core, 2);
    EXPECT_TRUE(spin_wait_ge(g_markers_fake.released_count, 3));

    int32_t ends = s.new_buffer(3 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    int32_t last_probe = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(ends >= 0 && last_probe >= 0);
    gpu::memory* last_probe_memory = g_markers_fake.last_created;
    ASSERT_EQ(s.map(q.context, ends, FAKE_VA_START, 0, 3 * PAGE, 0), static_cast<int64_t>(0));
    close_handle(ends);

    EXPECT_EQ(q.submit(0x83, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START + 2 * PAGE, PAGE), static_cast<int64_t>(0));
    gpu::progress(q.queue->core, 3);
    EXPECT_EQ(q.submit(0x84, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START, PAGE), static_cast<int64_t>(0));
    gpu::progress(q.queue->core, 4);

    close_handle(last_probe);
    EXPECT_TRUE(spin_wait_ge(g_markers_fake.released_count, 4));
    EXPECT_TRUE(g_markers_fake.last_released == last_probe_memory);
    EXPECT_EQ(g_markers_fake.released_count.load_acquire(), 4u);
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START + PAGE, PAGE), static_cast<int64_t>(0));
    EXPECT_TRUE(spin_wait_ge(g_markers_fake.released_count, 5));
}

static gpu_test::fake_backend g_marker_memory_fake;

// An unmap that cannot allocate its `unmap_marker` returns ENOMEM before the backend is asked and changes nothing
TEST(gpu_submission, an_unmap_without_memory_for_its_marker_changes_nothing) {
    session s(g_marker_memory_fake);
    ASSERT_TRUE(s.ready());
    int32_t context = s.new_context();
    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(context >= 0 && buffer >= 0);
    ASSERT_EQ(s.map(context, buffer, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));

    gpu::__dbg_test_fail_next_allocation();
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, PAGE), syscall::ENOMEM);
    EXPECT_EQ(g_marker_memory_fake.unmap_calls, 0u);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, 0, PAGE, 0), syscall::EBUSY);
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(g_marker_memory_fake.spaces[0].range_count, 0u);

    close_handle(buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_marker_memory_fake.released_count, 1));
}

static gpu_test::fake_backend g_failed_device_fake;

// A failed invalidate at an unmap fails the device: running work ends failed, the context reads lost to the device,
// and each call that would reach the GPU fails with EIO
TEST(gpu_submission, a_failed_device_ends_running_work_and_fails_later_calls) {
    g_failed_device_fake.fail_unmap = true;
    session s(g_failed_device_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_failed_device_fake);
    ASSERT_TRUE(q.ready());

    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);
    ASSERT_EQ(s.map(q.context, buffer, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));

    gpu_point done = point(q.timeline, 1);
    EXPECT_EQ(q.submit(0x11, nullptr, 0, &done, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START, PAGE), syscall::EIO);
    EXPECT_TRUE(spin_wait_ge(g_failed_device_fake.failures_reported, 1));

    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(1));
    EXPECT_EQ(flags_of(s, q.timeline), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(s.lost(q.context), static_cast<int64_t>(GPU_LOST_DEVICE));

    EXPECT_EQ(q.submit(0x12, nullptr, 0, nullptr, 0), syscall::EIO);
    EXPECT_EQ(s.new_context(), syscall::EIO);
    EXPECT_EQ(s.new_queue(q.context, GPU_QUEUE_COPY), syscall::EIO);
    EXPECT_EQ(g_failed_device_fake.queue_count, 1u);

    EXPECT_EQ(s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0), syscall::EIO);
    EXPECT_EQ(s.map(q.context, buffer, FAKE_VA_START, 0, PAGE, 0), syscall::EIO);
    EXPECT_EQ(s.unmap(q.context, FAKE_VA_START, PAGE), syscall::EIO);

    close_handle(buffer);
    EXPECT_TRUE(spin_wait_ge(g_failed_device_fake.released_count, 1));
    EXPECT_EQ(g_failed_device_fake.freed_count.load_acquire(), 0u);
}

static gpu_test::fake_backend g_zeroing_fake;

// A create whose zeroing fails returns EIO and fails the device: the context reads lost to the device, a later
// create fails before the backend, and a later release still reaches the backend, which keeps the memory
TEST(gpu_submission, a_failed_zeroing_fails_the_device) {
    session s(g_zeroing_fake);
    ASSERT_TRUE(s.ready());
    int32_t context = s.new_context();
    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(context >= 0 && buffer >= 0);

    g_zeroing_fake.fail_zeroing = true;
    EXPECT_EQ(s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0), syscall::EIO);
    EXPECT_TRUE(spin_wait_ge(g_zeroing_fake.failures_reported, 1));
    EXPECT_EQ(s.lost(context), static_cast<int64_t>(GPU_LOST_DEVICE));
    EXPECT_EQ(s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0), syscall::EIO);
    EXPECT_EQ(g_zeroing_fake.create_calls, 2u);

    close_handle(buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_zeroing_fake.released_count, 1));
    EXPECT_EQ(g_zeroing_fake.freed_count.load_acquire(), 0u);
}

static gpu_test::fake_backend g_stopping_failure_fake;

// A device failing while a stop is pending ends every queue's held and running work failed and asks no stop after,
// and the pending destroy's cleanup and the context's teardown still run
TEST(gpu_submission, a_device_failing_with_a_stop_pending_ends_the_work_of_every_queue) {
    g_stopping_failure_fake.stops_on_reaper = false;
    g_stopping_failure_fake.fail_unmap = true;
    session s(g_stopping_failure_fake);
    ASSERT_TRUE(s.ready());
    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);

    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));
    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_COPY), static_cast<int64_t>(1));
    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);
    ASSERT_EQ(s.map(context, buffer, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));

    int32_t gate = s.new_timeline(0);
    int32_t stopping = s.new_timeline(0);
    int32_t running = s.new_timeline(0);
    int32_t held = s.new_timeline(0);
    ASSERT_TRUE(gate >= 0 && stopping >= 0 && running >= 0 && held >= 0);

    gpu_point stopping_point = point(stopping, 1);
    gpu_point running_point = point(running, 1);
    gpu_point gate_point = point(gate, 1);
    gpu_point held_point = point(held, 1);
    EXPECT_EQ(s.submit(context, 0, 0x91, SMALL, nullptr, 0, &stopping_point, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.destroy_queue(context, 0), static_cast<int64_t>(0));

    EXPECT_EQ(s.submit(context, 1, 0x92, SMALL, nullptr, 0, &running_point, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.submit(context, 1, 0x93, SMALL, &gate_point, 1, &held_point, 1), static_cast<int64_t>(0));
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, PAGE), syscall::EIO);
    EXPECT_TRUE(spin_wait_ge(g_stopping_failure_fake.failures_reported, 1));
    EXPECT_TRUE(spin_wait_ge(g_stopping_failure_fake.queues_destroyed, 1));

    EXPECT_EQ(flags_of(s, stopping), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(flags_of(s, running), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(flags_of(s, held), static_cast<uint64_t>(GPU_TIMELINE_FAILED));
    EXPECT_EQ(s.signal(gate, 1), static_cast<int64_t>(0));
    EXPECT_EQ(g_stopping_failure_fake.queues[1].runs, 1u);

    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_stopping_failure_fake.spaces_destroyed, 1));
    EXPECT_EQ(g_stopping_failure_fake.queues_destroyed.load_acquire(), 2u);
    EXPECT_EQ(g_stopping_failure_fake.stops_asked.load_acquire(), 1u);

    close_handle(buffer);
    close_handle(gate);
    close_handle(stopping);
    close_handle(running);
    close_handle(held);
}

static gpu_test::fake_backend g_submit_arguments_fake;
static gpu_test::fake_backend g_foreign_device_fake;

// Submit takes each limit but refuses one past it, nonzero padding, a queue never created, another device's context,
// a handle without its right, a list the backend refuses and a full queue, and a failed allocation changes nothing
TEST(gpu_submission, submit_refuses_each_invalid_argument_and_a_full_queue) {
    session s(g_submit_arguments_fake);
    ASSERT_TRUE(s.ready());
    queue_setup q(s, g_submit_arguments_fake);
    ASSERT_TRUE(q.ready());
    session other(g_foreign_device_fake);
    ASSERT_TRUE(other.ready());

    EXPECT_EQ(other.submit(q.context, 0, 0, SMALL, nullptr, 0, nullptr, 0), syscall::EINVAL);
    EXPECT_EQ(g_submit_arguments_fake.check_calls + g_foreign_device_fake.check_calls, 0u);
    EXPECT_EQ(s.submit(q.context, 0, 0, GPU_COMMANDS_BYTES_MAX + 1, nullptr, 0, nullptr, 0), syscall::EINVAL);
    EXPECT_EQ(s.submit(q.context, 0, 0, SMALL, nullptr, GPU_POINTS_MAX + 1, nullptr, 0), syscall::EINVAL);
    EXPECT_EQ(s.submit(q.context, 0, 0, SMALL, nullptr, 0, nullptr, GPU_POINTS_MAX + 1), syscall::EINVAL);
    EXPECT_EQ(s.submit(q.context, 1, 0, SMALL, nullptr, 0, nullptr, 0), syscall::EINVAL);

    gpu_point padded = point(q.timeline, 1);
    padded.padding = 1;
    EXPECT_EQ(q.submit(0, &padded, 1, nullptr, 0), syscall::EINVAL);

    gpu_submit_args padded_submit = {};
    padded_submit.context = q.context;
    padded_submit.padding = 1;
    EXPECT_EQ(s.call(GPU_SUBMIT, padded_submit), syscall::EINVAL);

    int32_t reader = s.restrict_to(q.timeline, GPU_RIGHT_READ);
    int32_t writer = s.restrict_to(q.timeline, GPU_RIGHT_WRITE);
    int32_t context_reader = s.restrict_to(q.context, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0 && writer >= 0 && context_reader >= 0);
    uint32_t checks = g_submit_arguments_fake.check_calls;

    gpu_point through_reader = point(reader, 1);
    gpu_point through_writer = point(writer, 0);
    EXPECT_EQ(q.submit(0, nullptr, 0, &through_reader, 1), syscall::EBADF);
    EXPECT_EQ(q.submit(0, &through_writer, 1, nullptr, 0), syscall::EBADF);
    EXPECT_EQ(s.submit(context_reader, 0, 0, SMALL, nullptr, 0, nullptr, 0), syscall::EBADF);
    EXPECT_EQ(g_submit_arguments_fake.check_calls, checks);

    g_submit_arguments_fake.check_result = gpu::ERR_REFUSED;
    EXPECT_EQ(q.submit(0x5A, nullptr, 0, nullptr, 0), syscall::EINVAL);
    EXPECT_EQ(g_submit_arguments_fake.last_check_size, static_cast<size_t>(SMALL));
    EXPECT_EQ(g_submit_arguments_fake.last_check_first_byte, 0x5A);
    g_submit_arguments_fake.check_result = gpu::OK;
    EXPECT_EQ(q.queue->runs, 0u);

    for (uint32_t i = 0; i < GPU_QUEUE_PENDING_MAX; i++) {
        EXPECT_EQ(q.submit(0, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    }

    EXPECT_EQ(q.submit(0, nullptr, 0, nullptr, 0), syscall::EBUSY);
    gpu::progress(q.queue->core, 1);
    EXPECT_EQ(q.submit(0, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    gpu::progress(q.queue->core, GPU_QUEUE_PENDING_MAX + 1);

    gpu_point reached = point(q.timeline, 0);
    gpu_point first = point(q.timeline, 1);
    checks = g_submit_arguments_fake.check_calls;
    gpu::__dbg_test_fail_next_allocation();
    EXPECT_EQ(q.submit(0, &reached, 1, &first, 1), syscall::ENOMEM);
    EXPECT_EQ(g_submit_arguments_fake.check_calls, checks);

    EXPECT_EQ(q.queue->runs, GPU_QUEUE_PENDING_MAX + 1);
    EXPECT_EQ(q.submit(0, &reached, 1, &first, 1), static_cast<int64_t>(0));

    gpu_point all_reached[GPU_POINTS_MAX] = {};
    gpu_point rising[GPU_POINTS_MAX] = {};
    for (uint32_t i = 0; i < GPU_POINTS_MAX; i++) {
        all_reached[i] = reached;
        rising[i] = point(q.timeline, i + 2);
    }

    EXPECT_EQ(q.submit(0, all_reached, GPU_POINTS_MAX, rising, GPU_POINTS_MAX), static_cast<int64_t>(0));

    gpu_submit_args largest = {};
    largest.context = q.context;
    largest.commands = map_list(s, GPU_COMMANDS_BYTES_MAX);
    largest.commands_size = GPU_COMMANDS_BYTES_MAX;
    ASSERT_TRUE(largest.commands != 0);
    EXPECT_EQ(s.call(GPU_SUBMIT, largest), static_cast<int64_t>(0));

    EXPECT_EQ(q.queue->last_size, static_cast<size_t>(GPU_COMMANDS_BYTES_MAX));
    gpu::progress(q.queue->core, GPU_QUEUE_PENDING_MAX + 4);
    EXPECT_EQ(s.query(q.timeline), static_cast<uint64_t>(GPU_POINTS_MAX + 1));

    close_handle(reader);
    close_handle(writer);
    close_handle(context_reader);
}
