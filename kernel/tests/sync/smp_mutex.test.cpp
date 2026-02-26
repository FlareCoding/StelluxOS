#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "sync/mutex.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(smp_mutex);

constexpr uint32_t MAX_TEST_CPUS = 16;

// --- cross_cpu_mutual_exclusion ---
// Proves: mutex protects a shared counter across CPUs.
// One task per CPU, each does lock/counter++/unlock 500 times.

constexpr uint32_t ME_ITERS = 500;

static sync::mutex g_me_mtx;
static volatile uint32_t g_me_counter;
static volatile uint32_t g_me_done[MAX_TEST_CPUS] = {};

static void me_worker_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    for (uint32_t i = 0; i < ME_ITERS; i++) {
        RUN_ELEVATED({
            sync::mutex_lock(g_me_mtx);
            uint32_t val = __atomic_load_n(&g_me_counter, __ATOMIC_RELAXED);
            __atomic_store_n(&g_me_counter, val + 1, __ATOMIC_RELAXED);
            sync::mutex_unlock(g_me_mtx);
        });
    }
    __atomic_store_n(&g_me_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_mutex, cross_cpu_mutual_exclusion) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    g_me_mtx.init();
    g_me_counter = 0;
    for (uint32_t i = 0; i < cpus; i++) {
        g_me_done[i] = 0;
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < cpus; i++) {
            sched::task* t = sched::create_kernel_task(
                me_worker_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "smp_me");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i);
        }
    });

    for (uint32_t i = 0; i < cpus; i++) {
        ASSERT_TRUE(spin_wait(&g_me_done[i]));
    }

    EXPECT_EQ(__atomic_load_n(&g_me_counter, __ATOMIC_ACQUIRE),
              cpus * ME_ITERS);
}

// --- cross_cpu_lock_blocks ---
// Proves: mutex_lock blocks a task on a remote CPU until the holder unlocks.

static sync::mutex g_block_mtx;
static volatile uint32_t g_block_waiting;
static volatile uint32_t g_block_acquired;

static void block_remote_fn(void*) {
    __atomic_store_n(&g_block_waiting, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sync::mutex_lock(g_block_mtx);
        __atomic_store_n(&g_block_acquired, 1, __ATOMIC_RELEASE);
        sync::mutex_unlock(g_block_mtx);
    });
    sched::exit(0);
}

TEST(smp_mutex, cross_cpu_lock_blocks) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_block_mtx.init();
    g_block_waiting = 0;
    g_block_acquired = 0;

    RUN_ELEVATED({
        sync::mutex_lock(g_block_mtx);
    });

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            block_remote_fn, nullptr, "smp_block");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(&g_block_waiting));
    brief_delay();

    EXPECT_EQ(__atomic_load_n(&g_block_acquired, __ATOMIC_ACQUIRE), 0u);

    RUN_ELEVATED({
        sync::mutex_unlock(g_block_mtx);
    });

    EXPECT_TRUE(spin_wait(&g_block_acquired));
}

// --- cross_cpu_trylock ---
// Proves: trylock fails from a remote CPU when the mutex is held,
// and succeeds once the holder releases.

static sync::mutex g_try_mtx;
static volatile uint32_t g_try_result_held;
static volatile uint32_t g_try_phase1_done;
static volatile uint32_t g_try_result_free;
static volatile uint32_t g_try_phase2_go;
static volatile uint32_t g_try_phase2_done;

static void try_remote_fn(void*) {
    bool got_it = false;
    RUN_ELEVATED({
        got_it = sync::mutex_trylock(g_try_mtx);
    });
    __atomic_store_n(&g_try_result_held, got_it ? 1 : 0, __ATOMIC_RELEASE);
    if (got_it) {
        RUN_ELEVATED({ sync::mutex_unlock(g_try_mtx); });
    }
    __atomic_store_n(&g_try_phase1_done, 1, __ATOMIC_RELEASE);

    while (!__atomic_load_n(&g_try_phase2_go, __ATOMIC_ACQUIRE)) {}

    RUN_ELEVATED({
        got_it = sync::mutex_trylock(g_try_mtx);
    });
    __atomic_store_n(&g_try_result_free, got_it ? 1 : 0, __ATOMIC_RELEASE);
    if (got_it) {
        RUN_ELEVATED({ sync::mutex_unlock(g_try_mtx); });
    }
    __atomic_store_n(&g_try_phase2_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_mutex, cross_cpu_trylock) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_try_mtx.init();
    g_try_result_held = 0;
    g_try_phase1_done = 0;
    g_try_result_free = 0;
    g_try_phase2_go = 0;
    g_try_phase2_done = 0;

    RUN_ELEVATED({
        sync::mutex_lock(g_try_mtx);
    });

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            try_remote_fn, nullptr, "smp_try");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(&g_try_phase1_done));
    EXPECT_EQ(__atomic_load_n(&g_try_result_held, __ATOMIC_ACQUIRE), 0u);

    RUN_ELEVATED({
        sync::mutex_unlock(g_try_mtx);
    });

    __atomic_store_n(&g_try_phase2_go, 1, __ATOMIC_RELEASE);

    ASSERT_TRUE(spin_wait(&g_try_phase2_done));
    EXPECT_EQ(__atomic_load_n(&g_try_result_free, __ATOMIC_ACQUIRE), 1u);
}

// --- cross_cpu_mutex_stress ---
// Proves: mutex holds under high cross-CPU contention.
// 2 tasks per CPU, each does 500 lock/counter++/unlock cycles.

constexpr uint32_t STRESS_PER_CPU = 2;
constexpr uint32_t STRESS_ITERS = 500;
static sync::mutex g_stress_mtx;
static volatile uint32_t g_stress_counter;
static volatile uint32_t g_stress_done_count;

static void stress_worker_fn(void*) {
    for (uint32_t i = 0; i < STRESS_ITERS; i++) {
        RUN_ELEVATED({
            sync::mutex_lock(g_stress_mtx);
            uint32_t val = __atomic_load_n(&g_stress_counter, __ATOMIC_RELAXED);
            __atomic_store_n(&g_stress_counter, val + 1, __ATOMIC_RELAXED);
            sync::mutex_unlock(g_stress_mtx);
        });
    }
    __atomic_fetch_add(&g_stress_done_count, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(smp_mutex, cross_cpu_mutex_stress) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_stress_mtx.init();
    g_stress_counter = 0;
    g_stress_done_count = 0;

    uint32_t total_tasks = STRESS_PER_CPU * cpus;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < total_tasks; i++) {
            sched::task* t = sched::create_kernel_task(
                stress_worker_fn, nullptr, "smp_stress");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i % cpus);
        }
    });

    ASSERT_TRUE(spin_wait_ge(&g_stress_done_count, total_tasks));
    EXPECT_EQ(__atomic_load_n(&g_stress_counter, __ATOMIC_ACQUIRE),
              total_tasks * STRESS_ITERS);
}
