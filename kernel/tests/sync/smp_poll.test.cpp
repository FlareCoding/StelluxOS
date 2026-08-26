#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "sync/poll.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(smp_poll);

// cross_cpu_trigger
// Polling task on CPU 1, source fired from CPU 0.

static sync::wait_queue g_xcpu_wq;
static sync::atomic<uint32_t> g_xcpu_waiting;
static sync::atomic<uint32_t> g_xcpu_result;
static sync::atomic<uint32_t> g_xcpu_done;

static void xcpu_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_xcpu_wq);

        g_xcpu_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, 0);
        g_xcpu_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_xcpu_done.store_release(1);
    sched::exit(0);
}

TEST(smp_poll, cross_cpu_trigger) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xcpu_wq.init();
    g_xcpu_waiting.store_relaxed(0);
    g_xcpu_result.store_relaxed(0);
    g_xcpu_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(xcpu_poll_fn, nullptr, "smp_poll1");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(g_xcpu_waiting));
    brief_delay();

    // Fire from CPU 0 (BSP)
    RUN_ELEVATED({
        sync::wake_one(g_xcpu_wq);
    });

    ASSERT_TRUE(spin_wait(g_xcpu_done));
    EXPECT_EQ(g_xcpu_result.load_acquire(), 1u);
}

// cross_cpu_multi_source
// Polling task on CPU 1, 3 sources each fired from the BSP.

static sync::wait_queue g_xmulti_wq[3];
static sync::atomic<uint32_t> g_xmulti_waiting;
static sync::atomic<uint32_t> g_xmulti_result;
static sync::atomic<uint32_t> g_xmulti_done;

static void xmulti_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_xmulti_wq[i]);
        }

        g_xmulti_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, 0);
        g_xmulti_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_xmulti_done.store_release(1);
    sched::exit(0);
}

TEST(smp_poll, cross_cpu_multi_source) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    for (int i = 0; i < 3; i++) g_xmulti_wq[i].init();
    g_xmulti_waiting.store_relaxed(0);
    g_xmulti_result.store_relaxed(0);
    g_xmulti_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(xmulti_poll_fn, nullptr, "smp_pollm");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(g_xmulti_waiting));
    brief_delay();

    // Fire all 3 sources from BSP
    RUN_ELEVATED({
        sync::wake_one(g_xmulti_wq[0]);
        sync::wake_one(g_xmulti_wq[1]);
        sync::wake_one(g_xmulti_wq[2]);
    });

    ASSERT_TRUE(spin_wait(g_xmulti_done));
    EXPECT_EQ(g_xmulti_result.load_acquire(), 1u);
}

// cross_cpu_immediate_wake_preserves_scheduler_state
// Fire immediately after the waiter announces readiness so the wake races the
// BLOCKED transition. poll_wait must still return with the current task
// RUNNING and not leave sched_link queued.

static sync::wait_queue g_xfast_wq;
static sync::atomic<uint32_t> g_xfast_waiting;
static sync::atomic<uint32_t> g_xfast_done;
static sync::atomic<uint32_t> g_xfast_trigger_failures;
static sync::atomic<uint32_t> g_xfast_state_failures;
static sync::atomic<uint32_t> g_xfast_link_failures;

constexpr uint32_t XFAST_ITERS = 128;

static void xfast_poll_fn(void*) {
    for (uint32_t iter = 0; iter < XFAST_ITERS; iter++) {
        RUN_ELEVATED({
            sync::poll_table pt;
            pt.init(sched::current());
            sync::poll_subscribe(pt, g_xfast_wq);

            g_xfast_waiting.store_release(iter + 1);
            bool triggered = sync::poll_wait(pt, 0);

            sched::task* self = sched::current();
            if (!triggered) {
                g_xfast_trigger_failures.fetch_add_acq_rel(1);
            }
            if (self->state.load_relaxed() != sched::TASK_STATE_RUNNING) {
                g_xfast_state_failures.fetch_add_acq_rel(1);
            }
            if (self->sched_link.is_linked()) {
                g_xfast_link_failures.fetch_add_acq_rel(1);
            }

            sync::poll_cleanup(pt);
        });
        g_xfast_done.store_release(iter + 1);
    }
    sched::exit(0);
}

TEST(smp_poll, cross_cpu_immediate_wake_preserves_scheduler_state) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xfast_wq.init();
    g_xfast_waiting.store_relaxed(0);
    g_xfast_done.store_relaxed(0);
    g_xfast_trigger_failures.store_relaxed(0);
    g_xfast_state_failures.store_relaxed(0);
    g_xfast_link_failures.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(xfast_poll_fn, nullptr, "smp_pollf");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    for (uint32_t iter = 0; iter < XFAST_ITERS; iter++) {
        ASSERT_TRUE(spin_wait_ge(g_xfast_waiting, iter + 1));
        RUN_ELEVATED({
            sync::wake_one(g_xfast_wq);
        });
        ASSERT_TRUE(spin_wait_ge(g_xfast_done, iter + 1));
    }

    EXPECT_EQ(g_xfast_trigger_failures.load_acquire(), 0u);
    EXPECT_EQ(g_xfast_state_failures.load_acquire(), 0u);
    EXPECT_EQ(g_xfast_link_failures.load_acquire(), 0u);
}

// cross_cpu_cleanup_race
// Polling task on CPU 1 times out and enters cleanup.
// Source on CPU 0 fires concurrently. No crash, no corruption.

static sync::wait_queue g_xrace_wq;
static sync::atomic<uint32_t> g_xrace_done;

static void xrace_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_xrace_wq);

        // Short timeout, will expire before the source fires
        sync::poll_wait(pt, 10000000ULL); // 10ms
        sync::poll_cleanup(pt);
    });
    g_xrace_done.store_release(1);
    sched::exit(0);
}

TEST(smp_poll, cross_cpu_cleanup_race) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xrace_wq.init();
    g_xrace_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(xrace_poll_fn, nullptr, "smp_pollr");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    // Fire the source repeatedly while the poll task may be in cleanup
    for (int i = 0; i < 20; i++) {
        RUN_ELEVATED({
            sync::wake_one(g_xrace_wq);
        });
        brief_delay();
    }

    ASSERT_TRUE(spin_wait(g_xrace_done));
}
