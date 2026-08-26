#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_exec_core.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;

TEST_SUITE(preemption);

// --- basic_preemption ---
// Proves: timer preempts the boot task, created task runs without explicit yield.

static sync::atomic<uint32_t> g_basic_done;

static void basic_task_fn(void*) {
    g_basic_done.store_release(1);
    sched::exit(0);
}

TEST(preemption, basic_preemption) {
    g_basic_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(basic_task_fn, nullptr, "test_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    bool completed = spin_wait(g_basic_done);
    EXPECT_TRUE(completed);
}

// --- context_integrity ---
// Proves: register context survives preemption across many timer ticks.

static sync::atomic<uint32_t> g_fib_result;
static sync::atomic<uint32_t> g_fib_done;

static int fib(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    if (n == 2) return 1;
    return fib(n - 1) + fib(n - 2);
}

static void fib_task_fn(void*) {
    int result = fib(25);
    g_fib_result.store_release(static_cast<uint32_t>(result));
    g_fib_done.store_release(1);
    sched::exit(0);
}

TEST(preemption, context_integrity) {
    g_fib_result.store_relaxed(0);
    g_fib_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(fib_task_fn, nullptr, "test_fib");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    bool completed = spin_wait(g_fib_done);
    ASSERT_TRUE(completed);
    EXPECT_EQ(g_fib_result.load_acquire(), static_cast<uint32_t>(75025));
}

// --- multiple_tasks_complete ---
// Proves: round-robin scheduler doesn't starve any task.

constexpr uint32_t MULTI_COUNT = 4;
static sync::atomic<uint32_t> g_multi_done[MULTI_COUNT] = {};

static void multi_task_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    g_multi_done[idx].store_release(1);
    sched::exit(0);
}

TEST(preemption, multiple_tasks_complete) {
    for (uint32_t i = 0; i < MULTI_COUNT; i++) {
        g_multi_done[i].store_relaxed(0);
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < MULTI_COUNT; i++) {
            sched::task* t = sched::create_kernel_task(
                multi_task_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "test_multi");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    for (uint32_t i = 0; i < MULTI_COUNT; i++) {
        bool completed = spin_wait(g_multi_done[i]);
        EXPECT_TRUE(completed);
    }
}

// --- atomic_counter ---
// Proves: atomic operations work correctly under preemption, no lost updates.

constexpr uint32_t ATOMIC_TASKS = 4;
constexpr uint32_t ATOMIC_ITERS = 1000;
static sync::atomic<uint32_t> g_atomic_counter;
static sync::atomic<uint32_t> g_atomic_done[ATOMIC_TASKS] = {};

static void atomic_task_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    for (uint32_t i = 0; i < ATOMIC_ITERS; i++) {
        g_atomic_counter.fetch_add_relaxed(1);
    }
    g_atomic_done[idx].store_release(1);
    sched::exit(0);
}

TEST(preemption, atomic_counter) {
    g_atomic_counter.store_relaxed(0);
    for (uint32_t i = 0; i < ATOMIC_TASKS; i++) {
        g_atomic_done[i].store_relaxed(0);
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < ATOMIC_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                atomic_task_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "test_atomic");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    for (uint32_t i = 0; i < ATOMIC_TASKS; i++) {
        bool completed = spin_wait(g_atomic_done[i]);
        EXPECT_TRUE(completed);
    }

    uint32_t final_count = g_atomic_counter.load_acquire();
    EXPECT_EQ(final_count, ATOMIC_TASKS * ATOMIC_ITERS);
}
