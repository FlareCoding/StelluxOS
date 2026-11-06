#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "dynpriv/dynpriv.h"
#include "helpers.h"
#include "mm/kva.h"
#include "mm/page_quarantine.h"
#include "mm/paging.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sync/atomic.h"
#include "tests/memory/page_quarantine_test_access.h"

using page_quarantine::drain_claim_test_access;
using test_helpers::spin_wait;
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::stays_asleep;
using test_helpers::unpin;

TEST_SUITE(page_quarantine_test);

static sync::atomic<uint32_t> g_free_done{0};
static int32_t g_free_result = vmm::ERR_NOT_FOUND;

static void free_range(void* arg) {
    RUN_ELEVATED(g_free_result = vmm::free(reinterpret_cast<uintptr_t>(arg)));
    g_free_done.store_release(1);
    sched::exit(0);
}

TEST(page_quarantine_test, a_free_past_the_threshold_returns_while_a_drain_is_in_progress) {
    g_free_done.store_relaxed(0);

    size_t pages = page_quarantine::DRAIN_THRESHOLD_PAGES;
    uintptr_t range = 0;
    int32_t alloc_result = vmm::ERR_NO_MEM;
    RUN_ELEVATED(alloc_result = vmm::alloc(pages, paging::PAGE_KERNEL_RW, 0, kva::tag::generic, range));
    ASSERT_EQ(alloc_result, vmm::OK);

    drain_claim_test_access::hold();
    sched::task* freer = start_pinned_task_on_another_cpu(
        free_range,
        reinterpret_cast<void*>(range),
        "quarantine_free",
        sched::TASK_FLAG_ELEVATED
    );
    bool freed_while_held = freer && spin_wait(g_free_done);
    drain_claim_test_access::release();

    ASSERT_NOT_NULL(freer);
    EXPECT_TRUE(spin_wait(g_free_done));
    unpin(freer);

    EXPECT_TRUE(freed_while_held);
    EXPECT_EQ(g_free_result, vmm::OK);
}

static sync::atomic<uint32_t> g_drain_done{0};

static void drain_quarantine(void*) {
    RUN_ELEVATED(page_quarantine::drain());
    g_drain_done.store_release(1);
    sched::exit(0);
}

// A drain waiting for the one in progress sleeps until that one ends, even after a forced wake, and then finishes
TEST(page_quarantine_test, a_drain_sleeps_while_another_drain_is_in_progress) {
    g_drain_done.store_relaxed(0);

    drain_claim_test_access::hold();
    sched::task* waiter = start_pinned_task_on_another_cpu(
        drain_quarantine,
        nullptr,
        "quarantine_drain",
        sched::TASK_FLAG_ELEVATED
    );
    bool slept = waiter && stays_asleep(waiter);
    if (slept) {
        RUN_ELEVATED(sched::force_wake_for_kill(waiter));
    }

    bool slept_after_forced_wake = slept && stays_asleep(waiter);
    bool finished_while_held = g_drain_done.load_acquire() != 0;
    drain_claim_test_access::release();

    ASSERT_NOT_NULL(waiter);
    EXPECT_TRUE(spin_wait(g_drain_done));
    unpin(waiter);

    EXPECT_TRUE(slept);
    EXPECT_TRUE(slept_after_forced_wake);
    EXPECT_FALSE(finished_while_held);
}

static sync::atomic<uint32_t> g_claim_held{0};
static sync::atomic<uint32_t> g_runner_draining{0};
static sync::atomic<uint32_t> g_claim_releasing{0};

static void hold_claim_briefly(void*) {
    drain_claim_test_access::hold();
    g_claim_held.store_release(1);

    // The hold is timed from the runner's drain, so that the runner finds the claim still held
    (void)spin_wait(g_runner_draining);
    RUN_ELEVATED(sched::sleep_ms(50));

    g_claim_releasing.store_release(1);
    drain_claim_test_access::release();
    sched::exit(0);
}

// The runner is an idle task, whose drain must wait for the one in progress without sleeping
TEST(page_quarantine_test, an_idle_tasks_drain_runs_once_the_drain_in_progress_ends) {
    g_claim_held.store_relaxed(0);
    g_runner_draining.store_relaxed(0);
    g_claim_releasing.store_relaxed(0);

    sched::task* holder = start_pinned_task_on_another_cpu(
        hold_claim_briefly,
        nullptr,
        "quarantine_hold",
        sched::TASK_FLAG_ELEVATED
    );
    ASSERT_NOT_NULL(holder);

    bool holder_claimed = spin_wait(g_claim_held);
    g_runner_draining.store_release(1);
    RUN_ELEVATED(page_quarantine::drain());
    bool ran_after_release = g_claim_releasing.load_acquire() != 0;
    unpin(holder);

    EXPECT_TRUE(holder_claimed);
    EXPECT_TRUE(ran_after_release);
}
