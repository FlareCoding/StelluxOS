#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;

TEST_SUITE(smp_scheduling);

constexpr uint32_t MAX_TEST_CPUS = 16;

// --- enqueue_on_specific_cpu ---
// Proves: enqueue_on(t, N) causes the task to execute on CPU N.

static volatile uint32_t g_specific_cpu = 0xFFFFFFFF;
static volatile uint32_t g_specific_done = 0;

static void specific_cpu_fn(void*) {
    __atomic_store_n(&g_specific_cpu, percpu::current_cpu_id(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_specific_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_scheduling, enqueue_on_specific_cpu) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    uint32_t target = cpus - 1;
    g_specific_cpu = 0xFFFFFFFF;
    g_specific_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            specific_cpu_fn, nullptr, "smp_specific");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, target);
    });

    ASSERT_TRUE(spin_wait(&g_specific_done));
    EXPECT_EQ(__atomic_load_n(&g_specific_cpu, __ATOMIC_ACQUIRE), target);
}

// --- enqueue_on_all_cpus ---
// Proves: one task per CPU via enqueue_on, each runs on the correct CPU.

static volatile uint32_t g_all_cpu[MAX_TEST_CPUS] = {};
static volatile uint32_t g_all_done[MAX_TEST_CPUS] = {};

static void all_cpus_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    __atomic_store_n(&g_all_cpu[idx], percpu::current_cpu_id(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_all_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_scheduling, enqueue_on_all_cpus) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    for (uint32_t i = 0; i < cpus; i++) {
        g_all_cpu[i] = 0xFFFFFFFF;
        g_all_done[i] = 0;
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < cpus; i++) {
            sched::task* t = sched::create_kernel_task(
                all_cpus_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "smp_all");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i);
        }
    });

    for (uint32_t i = 0; i < cpus; i++) {
        ASSERT_TRUE(spin_wait(&g_all_done[i]));
        EXPECT_EQ(__atomic_load_n(&g_all_cpu[i], __ATOMIC_ACQUIRE), i);
    }
}

// --- load_balance_distributes ---
// Proves: enqueue() distributes tasks across multiple CPUs.

constexpr uint32_t LB_PER_CPU = 4;
constexpr uint32_t LB_MAX_TASKS = LB_PER_CPU * MAX_TEST_CPUS;

static volatile uint32_t g_lb_cpu[LB_MAX_TASKS] = {};
static volatile uint32_t g_lb_done[LB_MAX_TASKS] = {};

static void lb_task_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    __atomic_store_n(&g_lb_cpu[idx], percpu::current_cpu_id(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_lb_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_scheduling, load_balance_distributes) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    uint32_t task_count = LB_PER_CPU * cpus;
    for (uint32_t i = 0; i < task_count; i++) {
        g_lb_cpu[i] = 0xFFFFFFFF;
        g_lb_done[i] = 0;
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < task_count; i++) {
            sched::task* t = sched::create_kernel_task(
                lb_task_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "smp_lb");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    for (uint32_t i = 0; i < task_count; i++) {
        ASSERT_TRUE(spin_wait(&g_lb_done[i]));
    }

    uint32_t cpus_seen[MAX_TEST_CPUS] = {};
    for (uint32_t i = 0; i < task_count; i++) {
        uint32_t cpu = __atomic_load_n(&g_lb_cpu[i], __ATOMIC_ACQUIRE);
        if (cpu < cpus) {
            cpus_seen[cpu]++;
        }
    }

    uint32_t distinct = 0;
    for (uint32_t c = 0; c < cpus; c++) {
        if (cpus_seen[c] > 0) distinct++;
    }
    EXPECT_EQ(distinct, cpus);
}

// --- parallel_execution ---
// Proves: tasks on all CPUs run and complete (truly parallel execution).

static volatile uint32_t g_par_counter = 0;
static volatile uint32_t g_par_done[MAX_TEST_CPUS] = {};

static void par_task_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    __atomic_fetch_add(&g_par_counter, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_par_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_scheduling, parallel_execution) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    g_par_counter = 0;
    for (uint32_t i = 0; i < cpus; i++) {
        g_par_done[i] = 0;
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < cpus; i++) {
            sched::task* t = sched::create_kernel_task(
                par_task_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "smp_par");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i);
        }
    });

    for (uint32_t i = 0; i < cpus; i++) {
        ASSERT_TRUE(spin_wait(&g_par_done[i]));
    }

    EXPECT_EQ(__atomic_load_n(&g_par_counter, __ATOMIC_ACQUIRE), cpus);
}

// --- cross_cpu_atomic_counter ---
// Proves: atomic operations are coherent across CPUs.

constexpr uint32_t ATOMIC_ITERS = 1000;

static volatile uint32_t g_xatom_counter = 0;
static volatile uint32_t g_xatom_done[MAX_TEST_CPUS] = {};

static void xatom_task_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    for (uint32_t i = 0; i < ATOMIC_ITERS; i++) {
        __atomic_fetch_add(&g_xatom_counter, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_xatom_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_scheduling, cross_cpu_atomic_counter) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    g_xatom_counter = 0;
    for (uint32_t i = 0; i < cpus; i++) {
        g_xatom_done[i] = 0;
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < cpus; i++) {
            sched::task* t = sched::create_kernel_task(
                xatom_task_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "smp_atom");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i);
        }
    });

    for (uint32_t i = 0; i < cpus; i++) {
        ASSERT_TRUE(spin_wait(&g_xatom_done[i]));
    }

    EXPECT_EQ(__atomic_load_n(&g_xatom_counter, __ATOMIC_ACQUIRE),
              cpus * ATOMIC_ITERS);
}
