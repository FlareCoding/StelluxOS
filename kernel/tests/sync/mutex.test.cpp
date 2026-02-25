#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "sync/mutex.h"

TEST_SUITE(mutex);

constexpr uint64_t SPIN_TIMEOUT = 100000000;

static bool spin_wait(volatile uint32_t* flag) {
    uint64_t spins = 0;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (++spins > SPIN_TIMEOUT) return false;
    }
    return true;
}

static bool spin_wait_ge(volatile uint32_t* value, uint32_t target) {
    uint64_t spins = 0;
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < target) {
        if (++spins > SPIN_TIMEOUT) return false;
    }
    return true;
}

static void brief_delay() {
    uint64_t i = 0;
    while (i < 5000000) {
        asm volatile("" : "+r"(i));
        i++;
    }
}

// --- basic_lock_unlock ---

TEST(mutex, basic_lock_unlock) {
    sync::mutex m;
    m.init();

    EXPECT_FALSE(sync::mutex_is_locked(m));

    RUN_ELEVATED({
        sync::mutex_lock(m);
    });

    EXPECT_TRUE(sync::mutex_is_locked(m));

    RUN_ELEVATED({
        sync::mutex_unlock(m);
    });

    EXPECT_FALSE(sync::mutex_is_locked(m));
}

// --- trylock_when_free ---

TEST(mutex, trylock_when_free) {
    sync::mutex m;
    m.init();

    bool acquired = false;
    RUN_ELEVATED({
        acquired = sync::mutex_trylock(m);
    });

    EXPECT_TRUE(acquired);
    EXPECT_TRUE(sync::mutex_is_locked(m));

    RUN_ELEVATED({
        sync::mutex_unlock(m);
    });

    EXPECT_FALSE(sync::mutex_is_locked(m));
}

// --- trylock_when_held ---

static sync::mutex g_trylock_mtx;
static volatile uint32_t g_trylock_result;
static volatile uint32_t g_trylock_done;

static void trylock_task_fn(void*) {
    bool got_it = false;
    RUN_ELEVATED({
        got_it = sync::mutex_trylock(g_trylock_mtx);
    });
    __atomic_store_n(&g_trylock_result, got_it ? 1 : 0, __ATOMIC_RELEASE);
    if (got_it) {
        RUN_ELEVATED({
            sync::mutex_unlock(g_trylock_mtx);
        });
    }
    __atomic_store_n(&g_trylock_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(mutex, trylock_when_held) {
    g_trylock_mtx.init();
    g_trylock_result = 0;
    g_trylock_done = 0;

    RUN_ELEVATED({
        sync::mutex_lock(g_trylock_mtx);
    });

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            trylock_task_fn, nullptr, "mtx_try");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_trylock_done));
    EXPECT_EQ(__atomic_load_n(&g_trylock_result, __ATOMIC_ACQUIRE), 0u);

    RUN_ELEVATED({
        sync::mutex_unlock(g_trylock_mtx);
    });
}

// --- lock_blocks_until_unlock ---

static sync::mutex g_block_mtx;
static volatile uint32_t g_block_waiting;
static volatile uint32_t g_block_acquired;

static void block_waiter_fn(void*) {
    __atomic_store_n(&g_block_waiting, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sync::mutex_lock(g_block_mtx);
        __atomic_store_n(&g_block_acquired, 1, __ATOMIC_RELEASE);
        sync::mutex_unlock(g_block_mtx);
    });
    sched::exit(0);
}

TEST(mutex, lock_blocks_until_unlock) {
    g_block_mtx.init();
    g_block_waiting = 0;
    g_block_acquired = 0;

    RUN_ELEVATED({
        sync::mutex_lock(g_block_mtx);
    });

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            block_waiter_fn, nullptr, "mtx_block");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_block_waiting));
    brief_delay();

    EXPECT_EQ(__atomic_load_n(&g_block_acquired, __ATOMIC_ACQUIRE), 0u);

    RUN_ELEVATED({
        sync::mutex_unlock(g_block_mtx);
    });

    EXPECT_TRUE(spin_wait(&g_block_acquired));
}

// --- multiple_waiters_all_complete ---
// Test runner holds the mutex, creates 3 waiter tasks that block,
// then unlocks. Waiters cascade through (each unlock wakes the next).

constexpr uint32_t MW_TASKS = 3;

static sync::mutex g_mw_mtx;
static volatile uint32_t g_mw_done_count;

static void mw_waiter_fn(void*) {
    RUN_ELEVATED({
        sync::mutex_lock(g_mw_mtx);
        sync::mutex_unlock(g_mw_mtx);
    });
    __atomic_fetch_add(&g_mw_done_count, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(mutex, multiple_waiters_all_complete) {
    g_mw_mtx.init();
    g_mw_done_count = 0;

    RUN_ELEVATED({
        sync::mutex_lock(g_mw_mtx);
    });

    RUN_ELEVATED({
        for (uint32_t i = 0; i < MW_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                mw_waiter_fn, nullptr, "mtx_mw");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    brief_delay();
    EXPECT_EQ(__atomic_load_n(&g_mw_done_count, __ATOMIC_ACQUIRE), 0u);

    RUN_ELEVATED({
        sync::mutex_unlock(g_mw_mtx);
    });

    EXPECT_TRUE(spin_wait_ge(&g_mw_done_count, MW_TASKS));
}

// --- stress_mutual_exclusion ---

constexpr uint32_t STRESS_TASKS = 4;
constexpr uint32_t STRESS_ITERS = 1000;

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

TEST(mutex, stress_mutual_exclusion) {
    g_stress_mtx.init();
    g_stress_counter = 0;
    g_stress_done_count = 0;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < STRESS_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                stress_worker_fn, nullptr, "mtx_stress");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(&g_stress_done_count, STRESS_TASKS));
    EXPECT_EQ(__atomic_load_n(&g_stress_counter, __ATOMIC_ACQUIRE),
              STRESS_TASKS * STRESS_ITERS);
}
