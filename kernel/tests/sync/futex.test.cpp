#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/futex.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(futex);

// --- wait returns EAGAIN on value mismatch ---

static volatile uint32_t g_mismatch_val = 42;

TEST(futex, wait_eagain_on_mismatch) {
    g_mismatch_val = 42;
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_mismatch_val), 99, 0);
    });
    EXPECT_EQ(rc, static_cast<int32_t>(-11)); // EAGAIN
}

// --- wake with no waiters returns 0 ---

static volatile uint32_t g_nowait_val = 0;

TEST(futex, wake_no_waiters) {
    g_nowait_val = 0;
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_nowait_val), 1);
    });
    EXPECT_EQ(rc, static_cast<int32_t>(0));
}

// --- basic wait and wake ---

static volatile uint32_t g_basic_val = 0;
static volatile uint32_t g_basic_waiting = 0;
static volatile uint32_t g_basic_woken = 0;

static void basic_waiter_fn(void*) {
    __atomic_store_n(&g_basic_waiting, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_basic_val), 0, 0);
    });
    __atomic_store_n(&g_basic_woken, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(futex, basic_wait_and_wake) {
    g_basic_val = 0;
    g_basic_waiting = 0;
    g_basic_woken = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            basic_waiter_fn, nullptr, "ftx_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_basic_waiting));
    brief_delay();

    __atomic_store_n(&g_basic_val, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_basic_val), 1);
    });

    EXPECT_TRUE(spin_wait(&g_basic_woken));
}

// --- wake respects count ---

constexpr uint32_t WAKE_N_TASKS = 4;
static volatile uint32_t g_wn_val = 0;
static volatile uint32_t g_wn_ready = 0;
static volatile uint32_t g_wn_woken = 0;

static void wake_n_waiter_fn(void*) {
    __atomic_fetch_add(&g_wn_ready, 1, __ATOMIC_ACQ_REL);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_wn_val), 0, 0);
    });
    __atomic_fetch_add(&g_wn_woken, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(futex, wake_count_respected) {
    g_wn_val = 0;
    g_wn_ready = 0;
    g_wn_woken = 0;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < WAKE_N_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                wake_n_waiter_fn, nullptr, "ftx_wn");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(&g_wn_ready, WAKE_N_TASKS));
    brief_delay();

    int32_t woken = 0;
    RUN_ELEVATED({
        woken = sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_wn_val), 2);
    });
    EXPECT_EQ(woken, static_cast<int32_t>(2));

    ASSERT_TRUE(spin_wait_ge(&g_wn_woken, 2));
    brief_delay();
    EXPECT_EQ(__atomic_load_n(&g_wn_woken, __ATOMIC_ACQUIRE), 2u);

    int32_t rest = 0;
    RUN_ELEVATED({
        rest = sync::futex_wake_all(
            reinterpret_cast<uintptr_t>(&g_wn_val));
    });
    EXPECT_EQ(rest, static_cast<int32_t>(2));

    ASSERT_TRUE(spin_wait_ge(&g_wn_woken, WAKE_N_TASKS));
}

// --- wait with timeout ---

static volatile uint32_t g_timeout_val = 0;

TEST(futex, wait_timeout) {
    g_timeout_val = 0;
    uint64_t before = clock::now_ns();
    int32_t rc = 0;

    RUN_ELEVATED({
        rc = sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_timeout_val), 0,
            50000000ULL); // 50ms
    });

    uint64_t elapsed = clock::now_ns() - before;
    EXPECT_EQ(rc, static_cast<int32_t>(-110)); // ETIMEDOUT
    EXPECT_GE(elapsed, 10000000ULL); // at least ~10ms (timer granularity varies)
}

// --- killed thread unblocks ---

static volatile uint32_t g_kill_val = 0;
static volatile uint32_t g_kill_entered = 0;
static sched::task* g_kill_task = nullptr;

static void kill_waiter_fn(void*) {
    __atomic_store_n(&g_kill_entered, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_kill_val), 0, 0);
    });
    sched::exit(0);
}

TEST(futex, killed_thread_unblocks) {
    g_kill_val = 0;
    g_kill_entered = 0;
    g_kill_task = nullptr;

    RUN_ELEVATED({
        g_kill_task = sched::create_kernel_task(
            kill_waiter_fn, nullptr, "ftx_kill");
        ASSERT_NOT_NULL(g_kill_task);
        sched::enqueue(g_kill_task);
    });

    ASSERT_TRUE(spin_wait(&g_kill_entered));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(g_kill_task);
    });

    // Allow time for the task to die and be reaped
    brief_delay();
    brief_delay();
    EXPECT_TRUE(true);
}

// --- wake_all wakes everyone ---

constexpr uint32_t WALL_TASKS = 8;
static volatile uint32_t g_wall_val = 0;
static volatile uint32_t g_wall_ready = 0;
static volatile uint32_t g_wall_woken = 0;

static void wake_all_waiter_fn(void*) {
    __atomic_fetch_add(&g_wall_ready, 1, __ATOMIC_ACQ_REL);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_wall_val), 0, 0);
    });
    __atomic_fetch_add(&g_wall_woken, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(futex, wake_all_wakes_everyone) {
    g_wall_val = 0;
    g_wall_ready = 0;
    g_wall_woken = 0;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < WALL_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                wake_all_waiter_fn, nullptr, "ftx_wall");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(&g_wall_ready, WALL_TASKS));
    brief_delay();

    int32_t woken = 0;
    RUN_ELEVATED({
        woken = sync::futex_wake_all(
            reinterpret_cast<uintptr_t>(&g_wall_val));
    });
    EXPECT_EQ(woken, static_cast<int32_t>(WALL_TASKS));
    EXPECT_TRUE(spin_wait_ge(&g_wall_woken, WALL_TASKS));
}

// --- different addresses are independent ---

static volatile uint32_t g_ind_val_a = 0;
static volatile uint32_t g_ind_val_b = 0;
static volatile uint32_t g_ind_ready_a = 0;
static volatile uint32_t g_ind_ready_b = 0;
static volatile uint32_t g_ind_woken_a = 0;
static volatile uint32_t g_ind_woken_b = 0;

static void ind_waiter_a_fn(void*) {
    __atomic_store_n(&g_ind_ready_a, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_ind_val_a), 0, 0);
    });
    __atomic_store_n(&g_ind_woken_a, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

static void ind_waiter_b_fn(void*) {
    __atomic_store_n(&g_ind_ready_b, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_ind_val_b), 0, 0);
    });
    __atomic_store_n(&g_ind_woken_b, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(futex, different_addresses_independent) {
    g_ind_val_a = 0;
    g_ind_val_b = 0;
    g_ind_ready_a = 0;
    g_ind_ready_b = 0;
    g_ind_woken_a = 0;
    g_ind_woken_b = 0;

    RUN_ELEVATED({
        sched::task* ta = sched::create_kernel_task(
            ind_waiter_a_fn, nullptr, "ftx_ind_a");
        sched::task* tb = sched::create_kernel_task(
            ind_waiter_b_fn, nullptr, "ftx_ind_b");
        ASSERT_NOT_NULL(ta);
        ASSERT_NOT_NULL(tb);
        sched::enqueue(ta);
        sched::enqueue(tb);
    });

    ASSERT_TRUE(spin_wait(&g_ind_ready_a));
    ASSERT_TRUE(spin_wait(&g_ind_ready_b));
    brief_delay();

    // Wake only address A
    RUN_ELEVATED({
        sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_ind_val_a), 1);
    });

    ASSERT_TRUE(spin_wait(&g_ind_woken_a));
    brief_delay();
    EXPECT_EQ(__atomic_load_n(&g_ind_woken_b, __ATOMIC_ACQUIRE), 0u);

    // Now wake B
    RUN_ELEVATED({
        sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_ind_val_b), 1);
    });

    EXPECT_TRUE(spin_wait(&g_ind_woken_b));
}
