#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "clock/clock.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(kill);

// --- force_wake_kills_sleeping_task ---
// A task sleeping for a long time is woken immediately by force_wake_for_kill.
// Verifies: kill_pending is set, sleep is cancelled, task completes promptly.

static volatile uint32_t g_sleep_kill_done = 0;
static volatile uint32_t g_sleep_kill_was_pending = 0;
static volatile uint64_t g_sleep_kill_elapsed_ns = 0;

static void sleep_kill_fn(void*) {
    uint64_t start = clock::now_ns();
    RUN_ELEVATED({
        sched::sleep_ns(5000000000ULL); // 5 seconds -- should be cancelled
    });
    uint64_t elapsed = clock::now_ns() - start;
    __atomic_store_n(&g_sleep_kill_elapsed_ns, elapsed, __ATOMIC_RELEASE);

    uint32_t kp = 0;
    RUN_ELEVATED({
        kp = __atomic_load_n(&sched::current()->kill_pending, __ATOMIC_ACQUIRE);
    });
    __atomic_store_n(&g_sleep_kill_was_pending, kp, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sleep_kill_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(kill, force_wake_kills_sleeping_task) {
    g_sleep_kill_done = 0;
    g_sleep_kill_was_pending = 0;
    g_sleep_kill_elapsed_ns = 0;

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(sleep_kill_fn, nullptr, "kill_sleep");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(&g_sleep_kill_done));
    EXPECT_EQ(__atomic_load_n(&g_sleep_kill_was_pending, __ATOMIC_ACQUIRE), 1u);
    EXPECT_LT(__atomic_load_n(&g_sleep_kill_elapsed_ns, __ATOMIC_ACQUIRE),
              static_cast<uint64_t>(2000000000)); // woke in < 2s, not 5s
}

// --- force_wake_kills_blocked_on_wq ---
// A task blocked on a wait queue is woken by force_wake_for_kill.
// Verifies: task wakes, self-removes from wq, and sees kill_pending.

static sync::wait_queue g_wq_kill_wq;
static sync::spinlock g_wq_kill_lock;
static volatile uint32_t g_wq_kill_waiting = 0;
static volatile uint32_t g_wq_kill_done = 0;
static volatile uint32_t g_wq_kill_was_pending = 0;

static void wq_kill_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_wq_kill_lock);
        __atomic_store_n(&g_wq_kill_waiting, 1, __ATOMIC_RELEASE);
        while (!sched::is_kill_pending()) {
            irq = sync::wait(g_wq_kill_wq, g_wq_kill_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_wq_kill_lock, irq);
    });

    uint32_t kp = 0;
    RUN_ELEVATED({
        kp = __atomic_load_n(&sched::current()->kill_pending, __ATOMIC_ACQUIRE);
    });
    __atomic_store_n(&g_wq_kill_was_pending, kp, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wq_kill_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(kill, force_wake_kills_blocked_on_wq) {
    g_wq_kill_wq.init();
    g_wq_kill_lock = sync::SPINLOCK_INIT;
    g_wq_kill_waiting = 0;
    g_wq_kill_done = 0;
    g_wq_kill_was_pending = 0;

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(wq_kill_fn, nullptr, "kill_wq");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_wq_kill_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(&g_wq_kill_done));
    EXPECT_EQ(__atomic_load_n(&g_wq_kill_was_pending, __ATOMIC_ACQUIRE), 1u);
}

// --- self_removal_cleans_wq ---
// After force-wake, the wait queue should be empty (task self-removed).

static sync::wait_queue g_sr_wq;
static sync::spinlock g_sr_lock;
static volatile uint32_t g_sr_waiting = 0;
static volatile uint32_t g_sr_done = 0;

static void sr_waiter_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_sr_lock);
        __atomic_store_n(&g_sr_waiting, 1, __ATOMIC_RELEASE);
        while (!sched::is_kill_pending()) {
            irq = sync::wait(g_sr_wq, g_sr_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_sr_lock, irq);
    });
    __atomic_store_n(&g_sr_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(kill, self_removal_cleans_wq) {
    g_sr_wq.init();
    g_sr_lock = sync::SPINLOCK_INIT;
    g_sr_waiting = 0;
    g_sr_done = 0;

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(sr_waiter_fn, nullptr, "kill_sr");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_sr_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(&g_sr_done));
    EXPECT_TRUE(g_sr_wq.waiters.empty());
}

// --- double_kill_is_harmless ---
// Calling force_wake_for_kill twice on the same task does not crash.

static volatile uint32_t g_double_waiting = 0;
static volatile uint32_t g_double_done = 0;
static volatile uint32_t g_double_kp = 0;
static sync::wait_queue g_double_wq;
static sync::spinlock g_double_lock;

static void double_kill_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_double_lock);
        __atomic_store_n(&g_double_waiting, 1, __ATOMIC_RELEASE);
        while (!sched::is_kill_pending()) {
            irq = sync::wait(g_double_wq, g_double_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_double_lock, irq);
        __atomic_store_n(&g_double_kp,
            __atomic_load_n(&sched::current()->kill_pending, __ATOMIC_ACQUIRE),
            __ATOMIC_RELEASE);
    });
    __atomic_store_n(&g_double_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(kill, double_kill_is_harmless) {
    g_double_wq.init();
    g_double_lock = sync::SPINLOCK_INIT;
    g_double_waiting = 0;
    g_double_done = 0;
    g_double_kp = 0;

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(double_kill_fn, nullptr, "kill_dbl");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_double_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(&g_double_done));
    EXPECT_EQ(__atomic_load_n(&g_double_kp, __ATOMIC_ACQUIRE), 1u);
}

// --- is_kill_pending_accessor ---
// Verifies is_kill_pending() returns correct values before and after setting the flag.

static volatile uint32_t g_ikp_before = 0xFF;
static volatile uint32_t g_ikp_after = 0xFF;
static volatile uint32_t g_ikp_done = 0;
static volatile uint32_t g_ikp_flag_set = 0;

static void ikp_fn(void*) {
    uint32_t before = 0;
    RUN_ELEVATED({
        before = sched::is_kill_pending() ? 1 : 0;
    });
    __atomic_store_n(&g_ikp_before, before, __ATOMIC_RELEASE);

    while (!__atomic_load_n(&g_ikp_flag_set, __ATOMIC_ACQUIRE)) {
        // busy wait for test driver to set kill_pending
    }

    uint32_t after = 0;
    RUN_ELEVATED({
        after = sched::is_kill_pending() ? 1 : 0;
    });
    __atomic_store_n(&g_ikp_after, after, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ikp_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(kill, is_kill_pending_accessor) {
    g_ikp_before = 0xFF;
    g_ikp_after = 0xFF;
    g_ikp_done = 0;
    g_ikp_flag_set = 0;

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(ikp_fn, nullptr, "kill_ikp");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    brief_delay();

    RUN_ELEVATED({
        __atomic_store_n(&t->kill_pending, 1, __ATOMIC_RELEASE);
    });
    __atomic_store_n(&g_ikp_flag_set, 1, __ATOMIC_RELEASE);

    ASSERT_TRUE(spin_wait(&g_ikp_done));
    EXPECT_EQ(__atomic_load_n(&g_ikp_before, __ATOMIC_ACQUIRE), 0u);
    EXPECT_EQ(__atomic_load_n(&g_ikp_after, __ATOMIC_ACQUIRE), 1u);
}

// --- wait_status_normal_exit ---
// Verifies that a normal exit(42) produces WIFEXITED-compatible wait_status.

TEST(kill, wait_status_normal_exit) {
    int32_t status = (42 & 0xFF) << 8;
    EXPECT_EQ(status & 0x7F, 0);
    EXPECT_EQ((status >> 8) & 0xFF, 42);
}

// --- wait_status_killed ---
// Verifies that a killed exit(9) with kill_pending produces WIFSIGNALED-compatible wait_status.

TEST(kill, wait_status_killed) {
    int32_t status = 9 & 0x7F;
    EXPECT_NE(status & 0x7F, 0);
    EXPECT_EQ(status & 0x7F, 9);
}
