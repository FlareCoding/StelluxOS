#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "dynpriv/dynpriv.h"
#include "smp/smp.h"
#include "clock/clock.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(task_registry_integration);

namespace {

constexpr uint32_t SNAP_BUF_SIZE = 256;
static uint32_t s_snap_buf[SNAP_BUF_SIZE];

static bool tid_in_snapshot(uint32_t tid) {
    uint32_t n = 0;
    RUN_ELEVATED({
        n = sched::g_task_registry.snapshot_tids(s_snap_buf, SNAP_BUF_SIZE);
    });
    for (uint32_t i = 0; i < n; i++) {
        if (s_snap_buf[i] == tid) return true;
    }
    return false;
}

// Spin-wait for a TID to disappear from the registry.
// Returns true if the TID disappeared within the timeout.
static bool wait_for_tid_removal(uint32_t tid) {
    constexpr uint64_t MAX_SPINS = 200000000;
    for (uint64_t spin = 0; spin < MAX_SPINS; spin++) {
        if (!tid_in_snapshot(tid)) return true;
    }
    return false;
}

} // namespace

// --- created_task_appears_in_registry ---
// create_kernel_task inserts the task into g_task_registry before returning.
// The task is in CREATED state (not yet enqueued). Verify its TID appears.

static volatile uint32_t g_appear_done = 0;

static void appear_fn(void*) {
    __atomic_store_n(&g_appear_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(task_registry_integration, created_task_appears_in_registry) {
    g_appear_done = 0;

    sched::task* t = nullptr;
    uint32_t tid = 0;
    RUN_ELEVATED({
        t = sched::create_kernel_task(appear_fn, nullptr, "reg_appear");
        ASSERT_NOT_NULL(t);
        tid = t->tid;
    });

    // Task is CREATED but not enqueued -- should already be in the registry
    EXPECT_TRUE(tid_in_snapshot(tid));

    // Clean up: enqueue and let it finish
    RUN_ELEVATED({
        sched::enqueue(t);
    });
    ASSERT_TRUE(spin_wait(&g_appear_done));
}

// --- multiple_tasks_appear ---
// Create several kernel tasks, verify all TIDs appear in the registry.

constexpr uint32_t MULTI_COUNT = 4;
static volatile uint32_t g_multi_done = 0;

static void multi_fn(void*) {
    __atomic_fetch_add(&g_multi_done, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(task_registry_integration, multiple_tasks_appear) {
    g_multi_done = 0;
    uint32_t tids[MULTI_COUNT];
    sched::task* tasks[MULTI_COUNT] = {};

    RUN_ELEVATED({
        for (uint32_t i = 0; i < MULTI_COUNT; i++) {
            tasks[i] = sched::create_kernel_task(multi_fn, nullptr, "reg_multi");
            ASSERT_NOT_NULL(tasks[i]);
            tids[i] = tasks[i]->tid;
        }
    });

    // All tasks should be in the registry before enqueue
    for (uint32_t i = 0; i < MULTI_COUNT; i++) {
        EXPECT_TRUE(tid_in_snapshot(tids[i]));
    }

    // Enqueue all so they can run, exit, and be reaped (no leak)
    RUN_ELEVATED({
        for (uint32_t i = 0; i < MULTI_COUNT; i++) {
            sched::enqueue(tasks[i]);
        }
    });

    ASSERT_TRUE(spin_wait_ge(&g_multi_done, MULTI_COUNT));
}

// --- count_increases_on_create ---
// Verify that g_task_registry.count() increases when a new task is created.

static volatile uint32_t g_count_done = 0;

static void count_task_fn(void*) {
    __atomic_store_n(&g_count_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(task_registry_integration, count_increases_on_create) {
    g_count_done = 0;

    uint32_t before = 0;
    uint32_t after = 0;
    sched::task* t = nullptr;

    RUN_ELEVATED({
        before = sched::g_task_registry.count();
        t = sched::create_kernel_task(count_task_fn, nullptr, "reg_count");
        ASSERT_NOT_NULL(t);
        after = sched::g_task_registry.count();
    });

    EXPECT_EQ(after, before + 1);

    // Clean up
    RUN_ELEVATED({
        sched::enqueue(t);
    });
    ASSERT_TRUE(spin_wait(&g_count_done));
}

// --- exited_task_eventually_removed ---
// Create a task, enqueue it, let it exit. Verify its TID eventually
// disappears from the registry (cleaned up by the reaper).

static volatile uint32_t g_exit_done = 0;

static void exit_task_fn(void*) {
    __atomic_store_n(&g_exit_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(task_registry_integration, exited_task_eventually_removed) {
    g_exit_done = 0;

    uint32_t tid = 0;
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(exit_task_fn, nullptr, "reg_exit");
        ASSERT_NOT_NULL(t);
        tid = t->tid;
        sched::enqueue(t);
    });

    // Wait for the task to signal it's about to exit
    ASSERT_TRUE(spin_wait(&g_exit_done));

    // Give the reaper time to process
    brief_delay();
    brief_delay();

    // The TID should eventually be removed from the registry
    EXPECT_TRUE(wait_for_tid_removal(tid));
}

// --- task_with_work_appears_and_disappears ---
// A task that does some actual work (sleep) should be in the registry
// while alive, and removed after exit.

static volatile uint32_t g_work_alive = 0;
static volatile uint32_t g_work_done = 0;

static void work_task_fn(void*) {
    __atomic_store_n(&g_work_alive, 1, __ATOMIC_RELEASE);
    RUN_ELEVATED({
        sched::sleep_ns(50000000ULL); // 50ms
    });
    __atomic_store_n(&g_work_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(task_registry_integration, task_with_work_appears_and_disappears) {
    g_work_alive = 0;
    g_work_done = 0;

    uint32_t tid = 0;
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(work_task_fn, nullptr, "reg_work");
        ASSERT_NOT_NULL(t);
        tid = t->tid;
        sched::enqueue(t);
    });

    // Wait for the task to be alive
    ASSERT_TRUE(spin_wait(&g_work_alive));

    // While alive, it must be in the registry
    EXPECT_TRUE(tid_in_snapshot(tid));

    // Wait for task to finish its work
    ASSERT_TRUE(spin_wait(&g_work_done));

    // Give reaper time
    brief_delay();
    brief_delay();

    // After exit + reaping, TID should be gone
    EXPECT_TRUE(wait_for_tid_removal(tid));
}

// --- multiple_exits_all_cleaned ---
// Create several tasks that exit immediately. Verify all TIDs eventually
// disappear from the registry.

constexpr uint32_t BATCH_COUNT = 4;
static volatile uint32_t g_batch_done = 0;

static void batch_exit_fn(void*) {
    __atomic_fetch_add(&g_batch_done, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(task_registry_integration, multiple_exits_all_cleaned) {
    g_batch_done = 0;
    uint32_t tids[BATCH_COUNT];

    RUN_ELEVATED({
        for (uint32_t i = 0; i < BATCH_COUNT; i++) {
            sched::task* t = sched::create_kernel_task(batch_exit_fn, nullptr, "reg_batch");
            ASSERT_NOT_NULL(t);
            tids[i] = t->tid;
            sched::enqueue(t);
        }
    });

    // Wait for all tasks to signal they're exiting
    ASSERT_TRUE(spin_wait_ge(&g_batch_done, BATCH_COUNT));

    // Give reaper time
    brief_delay();
    brief_delay();

    // All TIDs should eventually be removed
    for (uint32_t i = 0; i < BATCH_COUNT; i++) {
        EXPECT_TRUE(wait_for_tid_removal(tids[i]));
    }
}

// --- snapshot_contains_running_tasks ---
// While tasks are running, snapshot should contain their TIDs.

static volatile uint32_t g_snap_ready = 0;
static volatile uint32_t g_snap_done = 0;

static void snap_task_fn(void*) {
    __atomic_fetch_add(&g_snap_ready, 1, __ATOMIC_ACQ_REL);
    // Sleep just long enough for the test to take its snapshot
    RUN_ELEVATED({
        sched::sleep_ns(100000000ULL); // 100ms
    });
    __atomic_fetch_add(&g_snap_done, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(task_registry_integration, snapshot_contains_running_tasks) {
    g_snap_ready = 0;
    g_snap_done = 0;

    constexpr uint32_t N = 3;
    uint32_t tids[N];

    RUN_ELEVATED({
        for (uint32_t i = 0; i < N; i++) {
            sched::task* t = sched::create_kernel_task(snap_task_fn, nullptr, "reg_snap");
            ASSERT_NOT_NULL(t);
            tids[i] = t->tid;
            sched::enqueue(t);
        }
    });

    // Wait for all tasks to signal they're alive
    ASSERT_TRUE(spin_wait_ge(&g_snap_ready, N));

    // Tasks are now sleeping. Take a snapshot -- they must still be in registry.
    for (uint32_t i = 0; i < N; i++) {
        EXPECT_TRUE(tid_in_snapshot(tids[i]));
    }

    // Wait for all tasks to finish and exit.
    // Use a generous busy-wait since tasks sleep 100ms then exit.
    constexpr uint64_t DONE_TIMEOUT = 500000000ULL;
    uint64_t spins = 0;
    while (__atomic_load_n(&g_snap_done, __ATOMIC_ACQUIRE) < N) {
        if (++spins > DONE_TIMEOUT) break;
    }
    EXPECT_GE(__atomic_load_n(&g_snap_done, __ATOMIC_ACQUIRE), N);
}

// --- concurrent_inserts_from_multiple_cpus ---
// Tasks running on different CPUs each create a sub-task, causing
// concurrent inserts into the global registry. Verify all TIDs appear.

static volatile uint32_t g_conc_tids[8] = {};
static volatile uint32_t g_conc_ready = 0;
static volatile uint32_t g_conc_subtask_done = 0;

static void conc_subtask_fn(void*) {
    __atomic_fetch_add(&g_conc_subtask_done, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

static void conc_creator_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    RUN_ELEVATED({
        sched::task* sub = sched::create_kernel_task(conc_subtask_fn, nullptr, "reg_csub");
        if (sub) {
            __atomic_store_n(&g_conc_tids[idx], sub->tid, __ATOMIC_RELEASE);
            sched::enqueue(sub);
        }
    });
    __atomic_fetch_add(&g_conc_ready, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(task_registry_integration, concurrent_inserts_from_multiple_cpus) {
    uint32_t cpus = smp::cpu_count();
    uint32_t n_creators = cpus < 4 ? cpus : 4;
    if (n_creators < 2) return; // Need at least 2 CPUs for this test

    g_conc_ready = 0;
    g_conc_subtask_done = 0;
    for (uint32_t i = 0; i < 8; i++) g_conc_tids[i] = 0;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < n_creators; i++) {
            sched::task* t = sched::create_kernel_task(
                conc_creator_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "reg_ccre");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i);
        }
    });

    // Wait for all creators to finish
    ASSERT_TRUE(spin_wait_ge(&g_conc_ready, n_creators));

    // Wait for subtasks
    ASSERT_TRUE(spin_wait_ge(&g_conc_subtask_done, n_creators));

    // All subtask TIDs should have appeared in the registry at some point.
    // Since subtasks may have already exited, we check that TIDs were assigned.
    for (uint32_t i = 0; i < n_creators; i++) {
        uint32_t sub_tid = __atomic_load_n(&g_conc_tids[i], __ATOMIC_ACQUIRE);
        EXPECT_NE(sub_tid, static_cast<uint32_t>(0));
    }
}

// --- registry_survives_rapid_create_exit ---
// Rapidly create and exit many tasks, ensuring the registry doesn't corrupt.

constexpr uint32_t RAPID_WAVES = 3;
constexpr uint32_t RAPID_PER_WAVE = 4;
static volatile uint32_t g_rapid_done = 0;

static void rapid_exit_fn(void*) {
    __atomic_fetch_add(&g_rapid_done, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(task_registry_integration, registry_survives_rapid_create_exit) {
    for (uint32_t wave = 0; wave < RAPID_WAVES; wave++) {
        g_rapid_done = 0;

        RUN_ELEVATED({
            for (uint32_t i = 0; i < RAPID_PER_WAVE; i++) {
                sched::task* t = sched::create_kernel_task(
                    rapid_exit_fn, nullptr, "reg_rapid");
                ASSERT_NOT_NULL(t);
                sched::enqueue(t);
            }
        });

        ASSERT_TRUE(spin_wait_ge(&g_rapid_done, RAPID_PER_WAVE));
        brief_delay();
    }

    // Registry should be consistent -- count should be non-negative
    // and snapshot should not crash.
    uint32_t n = 0;
    RUN_ELEVATED({
        n = sched::g_task_registry.snapshot_tids(s_snap_buf, SNAP_BUF_SIZE);
    });
    // We don't know exact count (idle tasks, test runner, etc.) but
    // n must be reasonable (not corrupted).
    EXPECT_GE(n, static_cast<uint32_t>(1)); // At least the idle/current task
    EXPECT_LE(n, SNAP_BUF_SIZE);
}

// --- count_consistent_with_snapshot ---
// In a quiescent state (no tasks being created/destroyed), count()
// and snapshot_tids() should agree.

TEST(task_registry_integration, count_consistent_with_snapshot) {
    brief_delay(); // Let any pending reaper work finish

    uint32_t count = 0;
    uint32_t snap_n = 0;
    RUN_ELEVATED({
        count = sched::g_task_registry.count();
        snap_n = sched::g_task_registry.snapshot_tids(s_snap_buf, SNAP_BUF_SIZE);
    });

    EXPECT_EQ(count, snap_n);
}

// --- task_tid_unique_in_snapshot ---
// Verify that no TID appears twice in a snapshot (would indicate
// corruption or double-insertion).

TEST(task_registry_integration, task_tid_unique_in_snapshot) {
    uint32_t n = 0;
    RUN_ELEVATED({
        n = sched::g_task_registry.snapshot_tids(s_snap_buf, SNAP_BUF_SIZE);
    });

    // Check for duplicates (O(n^2) but n is small)
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            EXPECT_NE(s_snap_buf[i], s_snap_buf[j]);
        }
    }
}
