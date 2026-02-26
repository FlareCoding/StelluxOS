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

static volatile uint32_t g_zero_done = 0;

static void zero_sleeper_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(0);
    });
    __atomic_store_n(&g_zero_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(sleep, sleep_ns_zero) {
    g_zero_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            zero_sleeper_fn, nullptr, "sleep_zero");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_zero_done));
}

// --- sleep_ms_basic ---
// Proves: sleep_ns(100ms) sleeps for approximately 100ms.

static volatile uint32_t g_basic_done = 0;
static volatile uint64_t g_basic_elapsed_hi = 0;
static volatile uint64_t g_basic_elapsed_lo = 0;

static void basic_sleeper_fn(void*) {
    uint64_t start = clock::now_ns();
    RUN_ELEVATED({
        sched::sleep_ns(100000000ULL);
    });
    uint64_t elapsed = clock::now_ns() - start;
    __atomic_store_n(&g_basic_elapsed_hi, elapsed >> 32, __ATOMIC_RELEASE);
    __atomic_store_n(&g_basic_elapsed_lo, elapsed & 0xFFFFFFFF, __ATOMIC_RELEASE);
    __atomic_store_n(&g_basic_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(sleep, sleep_ms_basic) {
    g_basic_done = 0;
    g_basic_elapsed_hi = 0;
    g_basic_elapsed_lo = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            basic_sleeper_fn, nullptr, "sleep_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_basic_done));

    uint64_t elapsed = (__atomic_load_n(&g_basic_elapsed_hi, __ATOMIC_ACQUIRE) << 32)
                     | __atomic_load_n(&g_basic_elapsed_lo, __ATOMIC_ACQUIRE);
    EXPECT_GE(elapsed, static_cast<uint64_t>(50000000));
    EXPECT_LE(elapsed, static_cast<uint64_t>(2000000000));
}

// --- sleep_ordering ---
// Proves: 50ms sleeper wakes before 100ms sleeper.

static volatile uint32_t g_order_count = 0;
static volatile uint32_t g_order_first = 0;

static void order_short_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(50000000ULL);
    });
    uint32_t idx = __atomic_fetch_add(&g_order_count, 1, __ATOMIC_ACQ_REL);
    if (idx == 0) __atomic_store_n(&g_order_first, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

static void order_long_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(150000000ULL);
    });
    uint32_t idx = __atomic_fetch_add(&g_order_count, 1, __ATOMIC_ACQ_REL);
    if (idx == 0) __atomic_store_n(&g_order_first, 2, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(sleep, sleep_ordering) {
    g_order_count = 0;
    g_order_first = 0;

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

    ASSERT_TRUE(spin_wait_ge(&g_order_count, 2));
    EXPECT_EQ(__atomic_load_n(&g_order_first, __ATOMIC_ACQUIRE), 1u);
}

// --- sleep_does_not_block_others ---
// Proves: a sleeping task does not prevent other tasks from running.

static volatile uint32_t g_runner_done = 0;
static volatile uint32_t g_sleeper_done = 0;

static void nonblock_runner_fn(void*) {
    __atomic_store_n(&g_runner_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

static void nonblock_sleeper_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(200000000ULL);
    });
    __atomic_store_n(&g_sleeper_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(sleep, sleep_does_not_block_others) {
    g_runner_done = 0;
    g_sleeper_done = 0;

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

    ASSERT_TRUE(spin_wait(&g_runner_done));
    ASSERT_TRUE(spin_wait(&g_sleeper_done));
}

// --- sleep_on_remote_cpu ---
// Proves: task on CPU 1 can sleep and wake correctly.

static volatile uint32_t g_remote_done = 0;
static volatile uint32_t g_remote_cpu = 0xFFFFFFFF;

static void remote_sleeper_fn(void*) {
    __atomic_store_n(&g_remote_cpu, percpu::current_cpu_id(), __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sched::sleep_ns(50000000ULL);
    });
    __atomic_store_n(&g_remote_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(sleep, sleep_on_remote_cpu) {
    if (smp::cpu_count() < 2) return;

    g_remote_done = 0;
    g_remote_cpu = 0xFFFFFFFF;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            remote_sleeper_fn, nullptr, "sleep_remote");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(&g_remote_done));
    EXPECT_EQ(__atomic_load_n(&g_remote_cpu, __ATOMIC_ACQUIRE), 1u);
}

// --- sleep_simultaneous ---
// Proves: tasks on all CPUs can sleep and wake independently.

static volatile uint32_t g_sim_done[MAX_TEST_CPUS] = {};

static void sim_sleeper_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    RUN_ELEVATED({
        sched::sleep_ns(100000000ULL);
    });
    __atomic_store_n(&g_sim_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(sleep, sleep_simultaneous) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    for (uint32_t i = 0; i < cpus; i++) {
        g_sim_done[i] = 0;
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
        ASSERT_TRUE(spin_wait(&g_sim_done[i]));
    }
}

// --- sleep_same_deadline ---
// Proves: 4 tasks with identical deadlines all complete.

static volatile uint32_t g_same_done_count = 0;

static void same_deadline_fn(void*) {
    RUN_ELEVATED({
        sched::sleep_ns(80000000ULL);
    });
    __atomic_fetch_add(&g_same_done_count, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(sleep, sleep_same_deadline) {
    g_same_done_count = 0;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < 4; i++) {
            sched::task* t = sched::create_kernel_task(
                same_deadline_fn, nullptr, "sleep_same");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(&g_same_done_count, 4));
}
