#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "fs/fs.h"
#include "fs/record_lock_table.h"
#include "mm/heap.h"
#include "sync/atomic.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(record_lock_table);

constexpr fs::record_lock_type SHARED = fs::record_lock_type::shared;
constexpr fs::record_lock_type EXCLUSIVE = fs::record_lock_type::exclusive;

// Owners are compared by address, so any distinct objects stand in for them
static char g_first_owner;
static char g_second_owner;
static char g_third_owner;

static fs::record_lock make_lock(const void* owner, fs::record_lock_type type, uint64_t start, uint64_t end) {
    return {owner, type, start, end, 0};
}

static int32_t try_lock(fs::record_lock_table& table, const fs::record_lock& request) {
    int32_t result = 0;
    RUN_ELEVATED(result = table.try_lock(request));
    return result;
}

static int32_t unlock(fs::record_lock_table& table, const void* owner, uint64_t start, uint64_t end) {
    int32_t result = 0;
    RUN_ELEVATED(result = table.unlock(owner, start, end));
    return result;
}

static void unlock_all(fs::record_lock_table& table, const void* owner) {
    RUN_ELEVATED(table.unlock_all(owner));
}

static bool find_conflict(
    fs::record_lock_table& table,
    const fs::record_lock& request,
    fs::record_lock* out_conflict
) {
    bool found = false;
    RUN_ELEVATED(found = table.find_conflict(request, out_conflict));
    return found;
}

TEST(record_lock_table, shared_locks_overlap_but_exclude_an_exclusive_lock) {
    fs::record_lock_table table;

    EXPECT_EQ(try_lock(table, make_lock(&g_first_owner, SHARED, 0, 99)), fs::OK);
    EXPECT_EQ(try_lock(table, make_lock(&g_second_owner, SHARED, 50, 149)), fs::OK);
    EXPECT_EQ(try_lock(table, make_lock(&g_third_owner, EXCLUSIVE, 90, 95)), fs::ERR_AGAIN);
    EXPECT_EQ(try_lock(table, make_lock(&g_third_owner, EXCLUSIVE, 150, 199)), fs::OK);
}

TEST(record_lock_table, an_owner_replaces_its_own_lock_without_conflict) {
    fs::record_lock_table table;
    ASSERT_EQ(try_lock(table, make_lock(&g_first_owner, EXCLUSIVE, 0, 99)), fs::OK);

    EXPECT_EQ(try_lock(table, make_lock(&g_first_owner, SHARED, 10, 19)), fs::OK);
    EXPECT_EQ(try_lock(table, make_lock(&g_second_owner, SHARED, 10, 19)), fs::OK);
    EXPECT_EQ(try_lock(table, make_lock(&g_second_owner, SHARED, 9, 9)), fs::ERR_AGAIN);
    EXPECT_EQ(try_lock(table, make_lock(&g_second_owner, SHARED, 20, 20)), fs::ERR_AGAIN);
}

TEST(record_lock_table, adjoining_locks_of_one_owner_merge) {
    fs::record_lock_table table;
    ASSERT_EQ(try_lock(table, make_lock(&g_first_owner, EXCLUSIVE, 0, 9)), fs::OK);
    ASSERT_EQ(try_lock(table, make_lock(&g_first_owner, EXCLUSIVE, 20, 29)), fs::OK);
    ASSERT_EQ(try_lock(table, make_lock(&g_first_owner, EXCLUSIVE, 10, 19)), fs::OK);

    fs::record_lock conflict = {};
    ASSERT_TRUE(find_conflict(table, make_lock(&g_second_owner, SHARED, 15, 15), &conflict));
    EXPECT_EQ(conflict.start, static_cast<uint64_t>(0));
    EXPECT_EQ(conflict.end, static_cast<uint64_t>(29));
}

TEST(record_lock_table, unlocking_the_middle_of_a_lock_keeps_both_ends) {
    fs::record_lock_table table;
    ASSERT_EQ(try_lock(table, make_lock(&g_first_owner, EXCLUSIVE, 0, 99)), fs::OK);
    ASSERT_EQ(unlock(table, &g_first_owner, 40, 59), fs::OK);

    EXPECT_EQ(try_lock(table, make_lock(&g_second_owner, EXCLUSIVE, 40, 59)), fs::OK);

    fs::record_lock conflict = {};
    ASSERT_TRUE(find_conflict(table, make_lock(&g_third_owner, SHARED, 0, 39), &conflict));
    EXPECT_EQ(conflict.start, static_cast<uint64_t>(0));
    EXPECT_EQ(conflict.end, static_cast<uint64_t>(39));

    ASSERT_TRUE(find_conflict(table, make_lock(&g_third_owner, SHARED, 60, 99), &conflict));
    EXPECT_EQ(conflict.start, static_cast<uint64_t>(60));
    EXPECT_EQ(conflict.end, static_cast<uint64_t>(99));
}

TEST(record_lock_table, unlock_all_releases_only_that_owners_locks) {
    fs::record_lock_table table;
    ASSERT_EQ(try_lock(table, make_lock(&g_first_owner, EXCLUSIVE, 0, 9)), fs::OK);
    ASSERT_EQ(try_lock(table, make_lock(&g_first_owner, SHARED, 20, 29)), fs::OK);
    ASSERT_EQ(try_lock(table, make_lock(&g_second_owner, SHARED, 40, 49)), fs::OK);

    unlock_all(table, &g_first_owner);

    EXPECT_EQ(try_lock(table, make_lock(&g_third_owner, EXCLUSIVE, 0, 29)), fs::OK);
    EXPECT_EQ(try_lock(table, make_lock(&g_third_owner, EXCLUSIVE, 40, 49)), fs::ERR_AGAIN);
}

static sync::atomic<uint32_t> g_waiter_done;
static sync::atomic<int32_t> g_waiter_result;

static void wait_to_lock_first_byte(void* table) {
    auto* contended = static_cast<fs::record_lock_table*>(table);
    g_waiter_result.store_relaxed(contended->lock(make_lock(&g_second_owner, EXCLUSIVE, 0, 0)));
    g_waiter_done.store_release(1);
    sched::exit(0);
}

TEST(record_lock_table, a_waiting_lock_is_granted_once_the_conflict_is_released) {
    // On the heap, so a waiter left behind by a failed assertion never reaches a dead stack frame
    auto* table = heap::ualloc_new<fs::record_lock_table>();
    ASSERT_NOT_NULL(table);
    ASSERT_EQ(try_lock(*table, make_lock(&g_first_owner, EXCLUSIVE, 0, 0)), fs::OK);

    g_waiter_done.store_relaxed(0);
    sched::task* waiter = test_helpers::start_pinned_task(wait_to_lock_first_byte, table, "record_lock_wait");
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));
    EXPECT_EQ(g_waiter_done.load_acquire(), 0u);

    unlock_all(*table, &g_first_owner);
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), fs::OK);

    fs::record_lock holder = {};
    ASSERT_TRUE(find_conflict(*table, make_lock(&g_third_owner, SHARED, 0, 0), &holder));
    EXPECT_TRUE(holder.owner == &g_second_owner);

    test_helpers::unpin(waiter);
    heap::ufree_delete(table);
}
