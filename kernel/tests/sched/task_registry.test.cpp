#define STLX_TEST_TIER TIER_DS

#include "stlx_unit_test.h"
#include "sched/task_registry.h"
#include "sched/task.h"
#include "common/string.h"

TEST_SUITE(task_registry);

namespace {

// Mock task helpers. The task struct is large, so all test data is static
// to avoid kernel stack overflow.

constexpr uint32_t MAX_MOCK_TASKS = 64;

static sched::task_registry s_reg;
static sched::task s_tasks[MAX_MOCK_TASKS];

// Zero-initialize a mock task and set its TID. Only the tid and
// task_registry_link fields matter for registry operations.
static void init_mock_task(sched::task& t, uint32_t tid) {
    string::memset(&t, 0, sizeof(sched::task));
    t.tid = tid;
    t.task_registry_link = {};
}

// Re-initialize the static registry and all mock tasks for a clean test.
static void reset_registry() {
    s_reg.init();
}

// Check whether a specific TID is present in a buffer of TIDs.
static bool tid_in_buffer(const uint32_t* buf, uint32_t count, uint32_t tid) {
    for (uint32_t i = 0; i < count; i++) {
        if (buf[i] == tid) return true;
    }
    return false;
}

// Snapshot buffer (static to avoid stack pressure)
static uint32_t s_snap_buf[MAX_MOCK_TASKS];

} // namespace

// --- init / count ---

TEST(task_registry, empty_registry_count_zero) {
    reset_registry();
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));
}

// --- insert ---

TEST(task_registry, insert_single_task) {
    reset_registry();
    init_mock_task(s_tasks[0], 100);

    s_reg.insert(&s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(1));
}

TEST(task_registry, insert_multiple_tasks) {
    reset_registry();
    constexpr uint32_t N = 16;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 100 + i);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N);
}

TEST(task_registry, insert_preserves_tid_identity) {
    reset_registry();
    init_mock_task(s_tasks[0], 42);
    init_mock_task(s_tasks[1], 99);
    s_reg.insert(&s_tasks[0]);
    s_reg.insert(&s_tasks[1]);

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(2));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 42));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 99));
}

TEST(task_registry, insert_with_large_tid_values) {
    reset_registry();
    // Use TIDs at boundaries of uint32_t range
    uint32_t tids[] = {0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF};
    constexpr uint32_t N = 5;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], tids[i]);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N);

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, N);
    for (uint32_t i = 0; i < N; i++) {
        EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, tids[i]));
    }
}

// --- remove ---

TEST(task_registry, remove_single_task) {
    reset_registry();
    init_mock_task(s_tasks[0], 50);
    s_reg.insert(&s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(1));

    s_reg.remove(s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));
}

TEST(task_registry, remove_from_many) {
    reset_registry();
    constexpr uint32_t N = 8;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 200 + i);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N);

    // Remove the 4th task (tid=203)
    s_reg.remove(s_tasks[3]);
    EXPECT_EQ(s_reg.count(), N - 1);

    // Verify 203 is gone, others remain
    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, N - 1);
    EXPECT_FALSE(tid_in_buffer(s_snap_buf, n, 203));
    for (uint32_t i = 0; i < N; i++) {
        if (i == 3) continue;
        EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 200 + i));
    }
}

TEST(task_registry, remove_first_inserted) {
    reset_registry();
    for (uint32_t i = 0; i < 4; i++) {
        init_mock_task(s_tasks[i], 10 + i);
        s_reg.insert(&s_tasks[i]);
    }

    // Remove the first task inserted (tid=10)
    s_reg.remove(s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(3));

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_FALSE(tid_in_buffer(s_snap_buf, n, 10));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 11));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 12));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 13));
}

TEST(task_registry, remove_last_inserted) {
    reset_registry();
    for (uint32_t i = 0; i < 4; i++) {
        init_mock_task(s_tasks[i], 10 + i);
        s_reg.insert(&s_tasks[i]);
    }

    // Remove the last task inserted (tid=13)
    s_reg.remove(s_tasks[3]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(3));

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 10));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 11));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 12));
    EXPECT_FALSE(tid_in_buffer(s_snap_buf, n, 13));
}

TEST(task_registry, remove_not_inserted_is_safe) {
    reset_registry();
    // Insert one real task
    init_mock_task(s_tasks[0], 50);
    s_reg.insert(&s_tasks[0]);

    // Create a task that was never inserted (pprev is null)
    init_mock_task(s_tasks[1], 99);

    // Removing a never-inserted task must be safe (no crash, no corruption)
    s_reg.remove(s_tasks[1]);

    // Original task should still be present
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(1));
    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(1));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 50));
}

TEST(task_registry, remove_not_inserted_on_empty_registry) {
    reset_registry();
    init_mock_task(s_tasks[0], 1);

    // Remove from an empty registry -- must be safe
    s_reg.remove(s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));
}

TEST(task_registry, remove_all_tasks) {
    reset_registry();
    constexpr uint32_t N = 16;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 300 + i);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N);

    for (uint32_t i = 0; i < N; i++) {
        s_reg.remove(s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(0));
}

// --- reinsert ---

TEST(task_registry, insert_remove_reinsert) {
    reset_registry();
    init_mock_task(s_tasks[0], 77);
    s_reg.insert(&s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(1));

    s_reg.remove(s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));

    // Re-zero the link field (as create_kernel_task does for new tasks)
    s_tasks[0].task_registry_link = {};
    s_reg.insert(&s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(1));

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(1));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 77));
}

TEST(task_registry, reinsert_multiple_times) {
    reset_registry();
    init_mock_task(s_tasks[0], 55);

    for (uint32_t cycle = 0; cycle < 8; cycle++) {
        s_tasks[0].task_registry_link = {};
        s_reg.insert(&s_tasks[0]);
        EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(1));

        s_reg.remove(s_tasks[0]);
        EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));
    }
}

// --- snapshot_tids ---

TEST(task_registry, snapshot_tids_empty) {
    reset_registry();
    // Fill buffer with sentinel value
    for (uint32_t i = 0; i < MAX_MOCK_TASKS; i++) {
        s_snap_buf[i] = 0xDEADBEEF;
    }

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(0));
    // Buffer should be untouched
    EXPECT_EQ(s_snap_buf[0], static_cast<uint32_t>(0xDEADBEEF));
}

TEST(task_registry, snapshot_tids_single) {
    reset_registry();
    init_mock_task(s_tasks[0], 42);
    s_reg.insert(&s_tasks[0]);

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(1));
    EXPECT_EQ(s_snap_buf[0], static_cast<uint32_t>(42));
}

TEST(task_registry, snapshot_tids_multiple) {
    reset_registry();
    constexpr uint32_t N = 12;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 1000 + i);
        s_reg.insert(&s_tasks[i]);
    }

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, N);

    // Verify all TIDs are present (order is non-deterministic)
    for (uint32_t i = 0; i < N; i++) {
        EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 1000 + i));
    }
}

TEST(task_registry, snapshot_tids_capped_by_max) {
    reset_registry();
    constexpr uint32_t N = 8;
    constexpr uint32_t CAP = 4;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 500 + i);
        s_reg.insert(&s_tasks[i]);
    }

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, CAP);
    EXPECT_EQ(n, CAP);

    // All returned TIDs must be valid (one of the inserted TIDs)
    for (uint32_t i = 0; i < n; i++) {
        bool valid = (s_snap_buf[i] >= 500 && s_snap_buf[i] < 500 + N);
        EXPECT_TRUE(valid);
    }
}

TEST(task_registry, snapshot_tids_max_zero) {
    reset_registry();
    init_mock_task(s_tasks[0], 1);
    s_reg.insert(&s_tasks[0]);

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, 0);
    EXPECT_EQ(n, static_cast<uint32_t>(0));
}

TEST(task_registry, snapshot_tids_after_remove) {
    reset_registry();
    init_mock_task(s_tasks[0], 10);
    init_mock_task(s_tasks[1], 20);
    init_mock_task(s_tasks[2], 30);
    s_reg.insert(&s_tasks[0]);
    s_reg.insert(&s_tasks[1]);
    s_reg.insert(&s_tasks[2]);

    // Remove task with tid=20
    s_reg.remove(s_tasks[1]);

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(2));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 10));
    EXPECT_FALSE(tid_in_buffer(s_snap_buf, n, 20));
    EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 30));
}

// --- count accuracy ---

TEST(task_registry, count_tracks_insertions) {
    reset_registry();
    for (uint32_t i = 0; i < 16; i++) {
        init_mock_task(s_tasks[i], 400 + i);
        s_reg.insert(&s_tasks[i]);
        EXPECT_EQ(s_reg.count(), i + 1);
    }
}

TEST(task_registry, count_tracks_removals) {
    reset_registry();
    constexpr uint32_t N = 16;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 400 + i);
        s_reg.insert(&s_tasks[i]);
    }

    for (uint32_t i = 0; i < N; i++) {
        s_reg.remove(s_tasks[i]);
        EXPECT_EQ(s_reg.count(), N - 1 - i);
    }
}

TEST(task_registry, count_after_mixed_operations) {
    reset_registry();
    // Insert 8
    for (uint32_t i = 0; i < 8; i++) {
        init_mock_task(s_tasks[i], 600 + i);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(8));

    // Remove odd-indexed (indices 1,3,5,7)
    for (uint32_t i = 1; i < 8; i += 2) {
        s_reg.remove(s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(4));

    // Verify snapshot matches
    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(4));
    for (uint32_t i = 0; i < 8; i += 2) {
        EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 600 + i));
    }
    for (uint32_t i = 1; i < 8; i += 2) {
        EXPECT_FALSE(tid_in_buffer(s_snap_buf, n, 600 + i));
    }
}

// --- stress ---

TEST(task_registry, stress_insert_remove_even) {
    reset_registry();
    constexpr uint32_t N = MAX_MOCK_TASKS;

    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 700 + i);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N);

    // Remove all even-indexed tasks
    for (uint32_t i = 0; i < N; i += 2) {
        s_reg.remove(s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N / 2);

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, N / 2);

    for (uint32_t i = 0; i < N; i++) {
        if (i % 2 == 0) {
            EXPECT_FALSE(tid_in_buffer(s_snap_buf, n, 700 + i));
        } else {
            EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 700 + i));
        }
    }
}

TEST(task_registry, stress_insert_remove_all_reinsert) {
    reset_registry();
    constexpr uint32_t N = 32;

    // Insert all
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 800 + i);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N);

    // Remove all
    for (uint32_t i = 0; i < N; i++) {
        s_reg.remove(s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));

    // Re-insert all with new TIDs
    for (uint32_t i = 0; i < N; i++) {
        s_tasks[i].task_registry_link = {};
        s_tasks[i].tid = 900 + i;
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), N);

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, N);
    for (uint32_t i = 0; i < N; i++) {
        EXPECT_TRUE(tid_in_buffer(s_snap_buf, n, 900 + i));
        EXPECT_FALSE(tid_in_buffer(s_snap_buf, n, 800 + i));
    }
}

// --- snapshot consistency with count ---

TEST(task_registry, snapshot_count_consistent) {
    reset_registry();
    constexpr uint32_t N = 10;
    for (uint32_t i = 0; i < N; i++) {
        init_mock_task(s_tasks[i], 1100 + i);
        s_reg.insert(&s_tasks[i]);
    }

    // In a single-threaded context, count and snapshot should agree
    uint32_t count = s_reg.count();
    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(count, n);
    EXPECT_EQ(count, N);
}

// --- init resets state ---

TEST(task_registry, reinit_clears_all) {
    reset_registry();
    // Insert some tasks
    for (uint32_t i = 0; i < 4; i++) {
        init_mock_task(s_tasks[i], 1200 + i);
        s_reg.insert(&s_tasks[i]);
    }
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(4));

    // Re-init should clear everything
    s_reg.init();
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));

    uint32_t n = s_reg.snapshot_tids(s_snap_buf, MAX_MOCK_TASKS);
    EXPECT_EQ(n, static_cast<uint32_t>(0));
}

// --- pprev guard behavior ---

TEST(task_registry, pprev_null_after_remove) {
    reset_registry();
    init_mock_task(s_tasks[0], 60);
    s_reg.insert(&s_tasks[0]);

    // After insert, pprev should be non-null (task is linked)
    EXPECT_NOT_NULL(s_tasks[0].task_registry_link.pprev);

    s_reg.remove(s_tasks[0]);

    // After remove, pprev should be null (hashmap clears it)
    EXPECT_NULL(s_tasks[0].task_registry_link.pprev);
    EXPECT_NULL(s_tasks[0].task_registry_link.next);
}

TEST(task_registry, double_remove_is_safe) {
    reset_registry();
    init_mock_task(s_tasks[0], 70);
    s_reg.insert(&s_tasks[0]);
    s_reg.remove(s_tasks[0]);

    // Second remove should be safe (pprev is null after first remove)
    s_reg.remove(s_tasks[0]);
    EXPECT_EQ(s_reg.count(), static_cast<uint32_t>(0));
}
