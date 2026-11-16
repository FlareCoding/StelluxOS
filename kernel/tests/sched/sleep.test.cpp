#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "clock/clock.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;

TEST_SUITE(sleep);

constexpr uint32_t MAX_TEST_CPUS = 16;

// --- sleep_ns_zero ---
// Proves: sleep_ns(0) returns quickly (yields without blocking).

static sync::atomic<uint32_t> g_zero_done;

static void zero_sleeper_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(0);
    });
    g_zero_done.store_release(1);
    sched::exit(0);
}

TEST(sleep, sleep_ns_zero) {
    g_zero_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            zero_sleeper_fn, nullptr, "sleep_zero");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_zero_done));
}

// --- sleep_ms_basic ---
// Proves: sleep_ns(100ms) sleeps for approximately 100ms.

static sync::atomic<uint32_t> g_basic_done;
static sync::atomic<uint64_t> g_basic_elapsed_hi;
static sync::atomic<uint64_t> g_basic_elapsed_lo;

static void basic_sleeper_fn(void*) {
    uint64_t start = clock::now_ns();
    RUN_ELEVATED({
        sched::sleep_ns(100000000ULL);
    });
    uint64_t elapsed = clock::now_ns() - start;
    g_basic_elapsed_hi.store_release(elapsed >> 32);
    g_basic_elapsed_lo.store_release(elapsed & 0xFFFFFFFF);
    g_basic_done.store_release(1);
    sched::exit(0);
}

TEST(sleep, sleep_ms_basic) {
    g_basic_done.store_relaxed(0);
    g_basic_elapsed_hi.store_relaxed(0);
    g_basic_elapsed_lo.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            basic_sleeper_fn, nullptr, "sleep_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_basic_done));

    uint64_t elapsed = (g_basic_elapsed_hi.load_acquire() << 32)
                     | g_basic_elapsed_lo.load_acquire();
    EXPECT_GE(elapsed, static_cast<uint64_t>(50000000));
    EXPECT_LE(elapsed, static_cast<uint64_t>(2000000000));
}

// --- sleep_ordering ---
// Proves: 50ms sleeper wakes before 100ms sleeper.

static sync::atomic<uint32_t> g_order_count;
static sync::atomic<uint32_t> g_order_first;

static void order_short_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(50000000ULL);
    });
    uint32_t idx = g_order_count.fetch_add_acq_rel(1);
    if (idx == 0) g_order_first.store_release(1);
    sched::exit(0);
}

static void order_long_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(150000000ULL);
    });
    uint32_t idx = g_order_count.fetch_add_acq_rel(1);
    if (idx == 0) g_order_first.store_release(2);
    sched::exit(0);
}

TEST(sleep, sleep_ordering) {
    g_order_count.store_relaxed(0);
    g_order_first.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t_long = sched::create_kernel_task(
            order_long_fn, nullptr, "sleep_long");
        sched::task* t_short = sched::create_kernel_task(
            order_short_fn, nullptr, "sleep_short");
        ASSERT_NOT_NULL(t_long);
        ASSERT_NOT_NULL(t_short);
        sched::enqueue_on(t_long, 0);
        sched::enqueue_on(t_short, 0);
    });

    ASSERT_TRUE(spin_wait_ge(g_order_count, 2));
    EXPECT_EQ(g_order_first.load_acquire(), 1u);
}

// --- sleep_does_not_block_others ---
// Proves: a sleeping task does not prevent other tasks from running.

static sync::atomic<uint32_t> g_runner_done;
static sync::atomic<uint32_t> g_sleeper_done;

static void nonblock_runner_fn(void*) {
    g_runner_done.store_release(1);
    sched::exit(0);
}

static void nonblock_sleeper_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(200000000ULL);
    });
    g_sleeper_done.store_release(1);
    sched::exit(0);
}

TEST(sleep, sleep_does_not_block_others) {
    g_runner_done.store_relaxed(0);
    g_sleeper_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* sleeper = sched::create_kernel_task(
            nonblock_sleeper_fn, nullptr, "sleep_nb_s");
        sched::task* runner = sched::create_kernel_task(
            nonblock_runner_fn, nullptr, "sleep_nb_r");
        ASSERT_NOT_NULL(sleeper);
        ASSERT_NOT_NULL(runner);
        sched::enqueue(sleeper);
        sched::enqueue(runner);
    });

    ASSERT_TRUE(spin_wait(g_runner_done));
    ASSERT_TRUE(spin_wait(g_sleeper_done));
}

// --- sleep_on_remote_cpu ---
// Proves: task on CPU 1 can sleep and wake correctly.

static sync::atomic<uint32_t> g_remote_done;
static sync::atomic<uint32_t> g_remote_cpu{0xFFFFFFFF};

static void remote_sleeper_fn(void*) {
    g_remote_cpu.store_release(percpu::current_cpu_id());
    RUN_ELEVATED({
        sched::sleep_ns(50000000ULL);
    });
    g_remote_done.store_release(1);
    sched::exit(0);
}

TEST(sleep, sleep_on_remote_cpu) {
    if (smp::cpu_count() < 2) return;

    g_remote_done.store_relaxed(0);
    g_remote_cpu.store_relaxed(0xFFFFFFFF);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            remote_sleeper_fn, nullptr, "sleep_remote");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(g_remote_done));
    EXPECT_EQ(g_remote_cpu.load_acquire(), 1u);
}

// --- sleep_simultaneous ---
// Proves: tasks on all CPUs can sleep and wake independently.

static sync::atomic<uint32_t> g_sim_done[MAX_TEST_CPUS] = {};

static void sim_sleeper_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    RUN_ELEVATED({
        sched::sleep_ns(100000000ULL);
    });
    g_sim_done[idx].store_release(1);
    sched::exit(0);
}

TEST(sleep, sleep_simultaneous) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    for (uint32_t i = 0; i < cpus; i++) {
        g_sim_done[i].store_relaxed(0);
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < cpus; i++) {
            sched::task* t = sched::create_kernel_task(
                sim_sleeper_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "sleep_sim");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i);
        }
    });

    for (uint32_t i = 0; i < cpus; i++) {
        ASSERT_TRUE(spin_wait(g_sim_done[i]));
    }
}

// --- sleep_same_deadline ---
// Proves: 4 tasks with identical deadlines all complete.

static sync::atomic<uint32_t> g_same_done_count;

static void same_deadline_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(80000000ULL);
    });
    g_same_done_count.fetch_add_acq_rel(1);
    sched::exit(0);
}

TEST(sleep, sleep_same_deadline) {
    g_same_done_count.store_relaxed(0);

    RUN_ELEVATED({
        for (uint32_t i = 0; i < 4; i++) {
            sched::task* t = sched::create_kernel_task(
                same_deadline_fn, nullptr, "sleep_same");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(g_same_done_count, 4));
}

// Proves: sleep_until_ns sleeps until its deadline and not much longer, reporting no time left, and returns at once
// for a deadline already passed.

constexpr uint64_t UNTIL_SLEEP_NS = 50000000ULL;
constexpr uint64_t UNTIL_MAX_OVERRUN_NS = 2000000000ULL;

static sync::atomic<uint32_t> g_until_done;
static uint64_t g_until_deadline;
static uint64_t g_until_started;
static uint64_t g_until_woke;
static uint64_t g_until_left;

static void until_sleeper_fn(void*) {
    g_until_started = clock::now_ns();
    RUN_ELEVATED({
        g_until_left = sched::sleep_until_ns(g_until_deadline);
    });
    g_until_woke = clock::now_ns();
    g_until_done.store_release(1);
    sched::exit(0);
}

// Runs sleep_until_ns to `deadline` on a task of its own, false when the task did not start or return in time
static bool sleep_until_on_a_task(uint64_t deadline) {
    g_until_done.store_relaxed(0);
    g_until_left = 1;
    g_until_deadline = deadline;

    sched::task* t = test_helpers::start_pinned_task(until_sleeper_fn, nullptr, "sleep_until");
    if (!t) {
        return false;
    }

    bool finished = spin_wait(g_until_done);
    test_helpers::unpin(t);

    return finished;
}

TEST(sleep, sleep_until_ns_wakes_once_its_deadline_passes) {
    ASSERT_TRUE(sleep_until_on_a_task(clock::now_ns() + UNTIL_SLEEP_NS));

    EXPECT_EQ(g_until_left, static_cast<uint64_t>(0));
    EXPECT_GE(g_until_woke, g_until_deadline);
    EXPECT_LE(g_until_woke, g_until_deadline + UNTIL_MAX_OVERRUN_NS);
}

TEST(sleep, sleep_until_ns_returns_at_once_for_a_passed_deadline) {
    ASSERT_TRUE(sleep_until_on_a_task(clock::now_ns() - 1));

    EXPECT_EQ(g_until_left, static_cast<uint64_t>(0));
    EXPECT_LE(g_until_woke - g_until_started, UNTIL_MAX_OVERRUN_NS);
}

// Proves: a duration ending past UINT64_MAX nanoseconds since boot sleeps until a kill instead of wrapping.

static sync::atomic<uint32_t> g_endless_done;
static uint64_t g_endless_left;

static void endless_sleeper_fn(void*) {
    RUN_ELEVATED({
        g_endless_left = sched::sleep_ns(~0ULL);
    });
    g_endless_done.store_release(1);
    sched::exit(0);
}

TEST(sleep, sleep_ns_ending_past_the_clock_range_lasts_until_a_kill) {
    g_endless_done.store_relaxed(0);
    g_endless_left = 0;

    sched::task* t = test_helpers::start_pinned_task(endless_sleeper_fn, nullptr, "sleep_endless");
    ASSERT_NOT_NULL(t);
    bool slept = test_helpers::stays_asleep(t);
    RUN_ELEVATED(sched::force_wake_for_kill(t));
    bool finished = spin_wait(g_endless_done);
    test_helpers::unpin(t);

    EXPECT_TRUE(slept);
    ASSERT_TRUE(finished);
    EXPECT_GT(g_endless_left, static_cast<uint64_t>(0));
}
