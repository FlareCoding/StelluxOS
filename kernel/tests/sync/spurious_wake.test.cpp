#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/futex.h"
#include "sync/poll.h"
#include "sync/atomic.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

// Every wait must survive a wake it did not ask for, which a timer of an
// earlier wait can deliver late. Here the stray wake is delivered on purpose.

TEST_SUITE(spurious_wake);

using test_helpers::spin_wait;

constexpr uint64_t MS = 1000000ULL;
constexpr uint64_t WAIT_NS = 200 * MS;
constexpr uint32_t STRAY_WAKES = 3;

static sync::atomic<uint32_t> g_waiting;
static sync::atomic<uint32_t> g_done;
static sync::atomic<uint64_t> g_elapsed_ns;
static sync::atomic<int32_t>  g_result;
static uint32_t g_futex_word;

// Holds a reference on the task under test so a wait that ends early fails
// an assertion instead of leaving the test polling a freed task
static sched::task* g_pinned = nullptr;

static void unpin() {
    sched::task* t = g_pinned;
    g_pinned = nullptr;
    if (t) {
        RUN_ELEVATED({
            if (t->release()) {
                sched::task::ref_destroy(t);
            }
        });
    }
}

static bool wait_until_blocked(sched::task* t) {
    uint64_t give_up = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (t->state.load_acquire() != sched::TASK_STATE_BLOCKED) {
        if (clock::now_ns() > give_up) return false;
    }

    return true;
}

// Waits for the task to block, then wakes it a few times with pauses so each
// wake lands on a task that has blocked again
static void deliver_stray_wakes(sched::task* t) {
    for (uint32_t i = 0; i < STRAY_WAKES; i++) {
        ASSERT_TRUE(wait_until_blocked(t));
        RUN_ELEVATED(sched::wake(t));

        uint64_t pause = clock::now_ns() + 10 * MS;
        while (clock::now_ns() < pause) {
            cpu::relax();
        }
    }
}

static void reset() {
    unpin();
    g_waiting.store_relaxed(0);
    g_done.store_relaxed(0);
    g_elapsed_ns.store_relaxed(0);
    g_result.store_relaxed(0);
    g_futex_word = 0;
}

static sched::task* start(void (*fn)(void*), const char* name) {
    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(fn, nullptr, name);
        if (t) {
            t->add_ref();
            g_pinned = t;
            sched::enqueue(t);
        }
    });

    return t;
}

// Waits for the task to finish its wait and drops the reference
static bool finish() {
    bool done = spin_wait(g_done);
    unpin();
    return done;
}

static void sleeper_fn(void*) {
    uint64_t before = clock::now_ns();
    g_waiting.store_release(1);
    RUN_ELEVATED(g_result.store_relaxed(static_cast<int32_t>(sched::sleep_ns(WAIT_NS))));
    g_elapsed_ns.store_release(clock::now_ns() - before);
    g_done.store_release(1);
    sched::exit(0);
}

TEST(spurious_wake, sleep_runs_its_full_length) {
    reset();
    sched::task* t = start(sleeper_fn, "stray_sleep");
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(spin_wait(g_waiting));

    deliver_stray_wakes(t);

    ASSERT_TRUE(finish());
    EXPECT_GE(g_elapsed_ns.load_acquire(), WAIT_NS);
    EXPECT_EQ(g_result.load_acquire(), 0);
}

static void poller_fn(void*) {
    uint64_t before = clock::now_ns();
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());

        g_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, WAIT_NS);
        g_result.store_relaxed(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_elapsed_ns.store_release(clock::now_ns() - before);
    g_done.store_release(1);
    sched::exit(0);
}

TEST(spurious_wake, poll_times_out_only_when_the_timeout_passes) {
    reset();
    sched::task* t = start(poller_fn, "stray_poll");
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(spin_wait(g_waiting));

    deliver_stray_wakes(t);

    ASSERT_TRUE(finish());
    EXPECT_GE(g_elapsed_ns.load_acquire(), WAIT_NS);
    EXPECT_EQ(g_result.load_acquire(), 0);
}

static void futex_fn(void*) {
    uint64_t before = clock::now_ns();
    g_waiting.store_release(1);
    RUN_ELEVATED({
        g_result.store_relaxed(sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_futex_word), 0, WAIT_NS));
    });
    g_elapsed_ns.store_release(clock::now_ns() - before);
    g_done.store_release(1);
    sched::exit(0);
}

TEST(spurious_wake, futex_reports_timeout_only_after_its_timeout) {
    reset();
    sched::task* t = start(futex_fn, "stray_futex");
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(spin_wait(g_waiting));

    deliver_stray_wakes(t);

    ASSERT_TRUE(finish());
    EXPECT_GE(g_elapsed_ns.load_acquire(), WAIT_NS);
    EXPECT_EQ(g_result.load_acquire(), -110);
}

TEST(spurious_wake, futex_still_wakes_for_a_real_wake) {
    reset();
    sched::task* t = start(futex_fn, "stray_futex_real");
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(spin_wait(g_waiting));

    deliver_stray_wakes(t);
    ASSERT_TRUE(wait_until_blocked(t));
    RUN_ELEVATED((void)sync::futex_wake(reinterpret_cast<uintptr_t>(&g_futex_word), 1));

    ASSERT_TRUE(finish());
    EXPECT_LT(g_elapsed_ns.load_acquire(), WAIT_NS);
    EXPECT_EQ(g_result.load_acquire(), 0);
}
