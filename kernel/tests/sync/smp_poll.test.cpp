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

// ---------------------------------------------------------------------------
// cross_cpu_trigger
// Polling task on CPU 1, source fired from CPU 0.
// ---------------------------------------------------------------------------

static sync::wait_queue g_xcpu_wq;
static volatile uint32_t g_xcpu_waiting;
static volatile uint32_t g_xcpu_result;
static volatile uint32_t g_xcpu_done;

static void xcpu_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entry = {};
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_xcpu_wq, entry);

        __atomic_store_n(&g_xcpu_waiting, 1, __ATOMIC_RELEASE);
        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_xcpu_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_xcpu_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_poll, cross_cpu_trigger) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xcpu_wq.init();
    g_xcpu_waiting = 0;
    g_xcpu_result = 0;
    g_xcpu_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(xcpu_poll_fn, nullptr, "smp_poll1");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(&g_xcpu_waiting));
    brief_delay();

    // Fire from CPU 0 (BSP)
    RUN_ELEVATED({
        sync::wake_one(g_xcpu_wq);
    });

    ASSERT_TRUE(spin_wait(&g_xcpu_done));
    EXPECT_EQ(__atomic_load_n(&g_xcpu_result, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// cross_cpu_multi_source
// Polling task on CPU 1, 3 sources each fired from the BSP.
// ---------------------------------------------------------------------------

static sync::wait_queue g_xmulti_wq[3];
static volatile uint32_t g_xmulti_waiting;
static volatile uint32_t g_xmulti_result;
static volatile uint32_t g_xmulti_done;

static void xmulti_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entries[3] = {};
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_xmulti_wq[i], entries[i]);
        }

        __atomic_store_n(&g_xmulti_waiting, 1, __ATOMIC_RELEASE);
        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_xmulti_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_xmulti_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_poll, cross_cpu_multi_source) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    for (int i = 0; i < 3; i++) g_xmulti_wq[i].init();
    g_xmulti_waiting = 0;
    g_xmulti_result = 0;
    g_xmulti_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(xmulti_poll_fn, nullptr, "smp_pollm");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(&g_xmulti_waiting));
    brief_delay();

    // Fire all 3 sources from BSP
    RUN_ELEVATED({
        sync::wake_one(g_xmulti_wq[0]);
        sync::wake_one(g_xmulti_wq[1]);
        sync::wake_one(g_xmulti_wq[2]);
    });

    ASSERT_TRUE(spin_wait(&g_xmulti_done));
    EXPECT_EQ(__atomic_load_n(&g_xmulti_result, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// cross_cpu_cleanup_race
// Polling task on CPU 1 times out and enters cleanup.
// Source on CPU 0 fires concurrently. No crash, no corruption.
// ---------------------------------------------------------------------------

static sync::wait_queue g_xrace_wq;
static volatile uint32_t g_xrace_done;

static void xrace_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entry = {};
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_xrace_wq, entry);

        // Short timeout — will expire before the source fires
        sync::poll_wait(pt, 10000000ULL); // 10ms
        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_xrace_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_poll, cross_cpu_cleanup_race) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xrace_wq.init();
    g_xrace_done = 0;

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

    ASSERT_TRUE(spin_wait(&g_xrace_done));
}
