#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/file_lock_table.h"
#include "mm/heap.h"
#include "sync/atomic.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(whole_file_locks);

constexpr fs::whole_file_lock_mode SHARED = fs::whole_file_lock_mode::shared;
constexpr fs::whole_file_lock_mode EXCLUSIVE = fs::whole_file_lock_mode::exclusive;

// A table, three holders and a waiting task's request. The holders are open file descriptions with no node, which
// the table never reads. On the heap, so a waiter left behind by a failed assertion never reaches a dead stack frame.
struct whole_file_lock_fixture {
    fs::file_lock_table table;
    fs::file first{rc::strong_ref<fs::node>(), fs::O_RDONLY};
    fs::file second{rc::strong_ref<fs::node>(), fs::O_RDONLY};
    fs::file third{rc::strong_ref<fs::node>(), fs::O_RDONLY};

    fs::file*                waiting_holder = nullptr;
    fs::whole_file_lock_mode waiting_mode = SHARED;
};

static int32_t try_lock(whole_file_lock_fixture* fixture, fs::file& holder, fs::whole_file_lock_mode mode) {
    int32_t result = 0;
    RUN_ELEVATED(result = fixture->table.try_lock_whole_file(holder, mode));
    return result;
}

static void unlock(whole_file_lock_fixture* fixture, fs::file& holder) {
    RUN_ELEVATED(fixture->table.unlock_whole_file(holder));
}

TEST(whole_file_locks, shared_holders_coexist_and_exclude_an_exclusive_one) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);

    EXPECT_EQ(try_lock(fixture, fixture->first, SHARED), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->second, SHARED), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->third, EXCLUSIVE), fs::ERR_AGAIN);
    EXPECT_EQ(try_lock(fixture, fixture->third, SHARED), fs::OK);

    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, an_exclusive_holder_excludes_every_other_holder) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::OK);

    EXPECT_EQ(try_lock(fixture, fixture->second, SHARED), fs::ERR_AGAIN);
    EXPECT_EQ(try_lock(fixture, fixture->second, EXCLUSIVE), fs::ERR_AGAIN);
    EXPECT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::OK);

    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, unlock_releases_only_the_holders_own_lock) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::OK);

    unlock(fixture, fixture->second);
    EXPECT_EQ(try_lock(fixture, fixture->third, SHARED), fs::ERR_AGAIN);

    unlock(fixture, fixture->first);
    EXPECT_EQ(try_lock(fixture, fixture->third, EXCLUSIVE), fs::OK);

    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, a_holder_converts_between_shared_and_exclusive) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, SHARED), fs::OK);

    ASSERT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->second, SHARED), fs::ERR_AGAIN);

    ASSERT_EQ(try_lock(fixture, fixture->first, SHARED), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->second, SHARED), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->third, EXCLUSIVE), fs::ERR_AGAIN);

    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, a_failed_conversion_leaves_nothing_held) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, SHARED), fs::OK);
    ASSERT_EQ(try_lock(fixture, fixture->second, SHARED), fs::OK);

    EXPECT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::ERR_AGAIN);
    EXPECT_EQ(try_lock(fixture, fixture->second, EXCLUSIVE), fs::OK);

    heap::ufree_delete(fixture);
}

static sync::atomic<uint32_t> g_waiter_done;
static sync::atomic<int32_t> g_waiter_result;

static void wait_for_lock(void* arg) {
    auto* fixture = static_cast<whole_file_lock_fixture*>(arg);
    g_waiter_result.store_relaxed(fixture->table.lock_whole_file(*fixture->waiting_holder, fixture->waiting_mode));
    g_waiter_done.store_release(1);
    sched::exit(0);
}

// A pinned task asking the fixture's table for `holder` to hold `mode`, or null when it cannot start
static sched::task* start_waiting_lock(
    whole_file_lock_fixture* fixture,
    fs::file& holder,
    fs::whole_file_lock_mode mode
) {
    fixture->waiting_holder = &holder;
    fixture->waiting_mode = mode;
    g_waiter_done.store_relaxed(0);

    return test_helpers::start_pinned_task(wait_for_lock, fixture, "whole_file_lock_wait");
}

TEST(whole_file_locks, a_waiting_lock_is_granted_once_the_conflict_is_released) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::OK);

    sched::task* waiter = start_waiting_lock(fixture, fixture->second, SHARED);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));
    EXPECT_EQ(g_waiter_done.load_acquire(), 0u);

    unlock(fixture, fixture->first);
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->third, EXCLUSIVE), fs::ERR_AGAIN);

    test_helpers::unpin(waiter);
    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, a_downgrade_grants_a_waiting_shared_lock) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::OK);

    sched::task* waiter = start_waiting_lock(fixture, fixture->second, SHARED);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));

    ASSERT_EQ(try_lock(fixture, fixture->first, SHARED), fs::OK);
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), fs::OK);

    test_helpers::unpin(waiter);
    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, a_waiting_conversion_releases_its_shared_lock_first) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, SHARED), fs::OK);
    ASSERT_EQ(try_lock(fixture, fixture->second, SHARED), fs::OK);

    sched::task* waiter = start_waiting_lock(fixture, fixture->first, EXCLUSIVE);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));

    // Had the waiter kept its shared lock, this conversion would fail, and a blocking one would wait forever
    ASSERT_EQ(try_lock(fixture, fixture->second, EXCLUSIVE), fs::OK);

    // The conversion woke the waiter while a conflict remains, so it checks again and sleeps
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));
    EXPECT_EQ(g_waiter_done.load_acquire(), 0u);

    unlock(fixture, fixture->second);
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->third, SHARED), fs::ERR_AGAIN);

    test_helpers::unpin(waiter);
    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, a_waiting_holder_never_waits_on_its_own_lock) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->second, SHARED), fs::OK);

    sched::task* waiter = start_waiting_lock(fixture, fixture->first, EXCLUSIVE);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));

    // Another handle to the waiting description takes a shared lock for it meanwhile
    ASSERT_EQ(try_lock(fixture, fixture->first, SHARED), fs::OK);
    unlock(fixture, fixture->second);
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), fs::OK);
    EXPECT_EQ(try_lock(fixture, fixture->third, SHARED), fs::ERR_AGAIN);

    unlock(fixture, fixture->first);
    EXPECT_EQ(try_lock(fixture, fixture->third, EXCLUSIVE), fs::OK);

    test_helpers::unpin(waiter);
    heap::ufree_delete(fixture);
}

TEST(whole_file_locks, a_kill_ends_a_waiting_lock_without_granting_it) {
    auto* fixture = heap::ualloc_new<whole_file_lock_fixture>();
    ASSERT_NOT_NULL(fixture);
    ASSERT_EQ(try_lock(fixture, fixture->first, EXCLUSIVE), fs::OK);

    sched::task* waiter = start_waiting_lock(fixture, fixture->second, EXCLUSIVE);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));

    RUN_ELEVATED(sched::force_wake_for_kill(waiter));
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), fs::ERR_INTR);

    unlock(fixture, fixture->first);
    EXPECT_EQ(try_lock(fixture, fixture->third, EXCLUSIVE), fs::OK);

    test_helpers::unpin(waiter);
    heap::ufree_delete(fixture);
}
