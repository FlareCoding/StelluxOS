#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/futex.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"
#include "syscall/syscall_table.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(futex);

// --- wait returns EAGAIN on value mismatch ---

static uint32_t g_mismatch_val = 42;

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

static uint32_t g_nowait_val = 0;

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

static uint32_t g_basic_val = 0;
static sync::atomic<uint32_t> g_basic_waiting;
static sync::atomic<uint32_t> g_basic_woken;

static void basic_waiter_fn(void*) {
    g_basic_waiting.store_release(1);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_basic_val), 0, 0);
    });
    g_basic_woken.store_release(1);
    sched::exit(0);
}

TEST(futex, basic_wait_and_wake) {
    g_basic_val = 0;
    g_basic_waiting.store_relaxed(0);
    g_basic_woken.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            basic_waiter_fn, nullptr, "ftx_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_basic_waiting));
    brief_delay();

    sync::atomic_ref<uint32_t>{g_basic_val}.store_release(1);
    RUN_ELEVATED({
        sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_basic_val), 1);
    });

    EXPECT_TRUE(spin_wait(g_basic_woken));
}

// --- wake respects count ---

constexpr uint32_t WAKE_N_TASKS = 4;
static uint32_t g_wn_val = 0;
static sync::atomic<uint32_t> g_wn_ready;
static sync::atomic<uint32_t> g_wn_woken;

static void wake_n_waiter_fn(void*) {
    g_wn_ready.fetch_add_acq_rel(1);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_wn_val), 0, 0);
    });
    g_wn_woken.fetch_add_acq_rel(1);
    sched::exit(0);
}

TEST(futex, wake_count_respected) {
    g_wn_val = 0;
    g_wn_ready.store_relaxed(0);
    g_wn_woken.store_relaxed(0);

    RUN_ELEVATED({
        for (uint32_t i = 0; i < WAKE_N_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                wake_n_waiter_fn, nullptr, "ftx_wn");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(g_wn_ready, WAKE_N_TASKS));
    brief_delay();

    int32_t woken = 0;
    RUN_ELEVATED({
        woken = sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_wn_val), 2);
    });
    EXPECT_EQ(woken, static_cast<int32_t>(2));

    ASSERT_TRUE(spin_wait_ge(g_wn_woken, 2));
    brief_delay();
    EXPECT_EQ(g_wn_woken.load_acquire(), 2u);

    int32_t rest = 0;
    RUN_ELEVATED({
        rest = sync::futex_wake_all(
            reinterpret_cast<uintptr_t>(&g_wn_val));
    });
    EXPECT_EQ(rest, static_cast<int32_t>(2));

    ASSERT_TRUE(spin_wait_ge(g_wn_woken, WAKE_N_TASKS));
}

// --- wait with timeout ---

static uint32_t g_timeout_val = 0;
static sync::atomic<int32_t> g_timeout_rc;
static sync::atomic<uint64_t> g_timeout_elapsed;
static sync::atomic<uint32_t> g_timeout_done;

static void timeout_waiter_fn(void*) {
    uint64_t before = clock::now_ns();
    RUN_ELEVATED({
        g_timeout_rc.store_relaxed(sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_timeout_val), 0,
            50000000ULL)); // 50ms
    });
    g_timeout_elapsed.store_release(clock::now_ns() - before);
    g_timeout_done.store_release(1);
    sched::exit(0);
}

TEST(futex, wait_timeout) {
    g_timeout_val = 0;
    g_timeout_rc.store_relaxed(0);
    g_timeout_elapsed.store_relaxed(0);
    g_timeout_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            timeout_waiter_fn, nullptr, "ftx_tmo");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_timeout_done));
    EXPECT_EQ(g_timeout_rc.load_relaxed(),
              static_cast<int32_t>(-110)); // ETIMEDOUT
    EXPECT_GE(g_timeout_elapsed.load_relaxed(), 10000000ULL);
}

// --- killed thread unblocks ---

static uint32_t g_kill_val = 0;
static sync::atomic<uint32_t> g_kill_entered;
static sched::task* g_kill_task = nullptr;

static void kill_waiter_fn(void*) {
    g_kill_entered.store_release(1);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_kill_val), 0, 0);
    });
    sched::exit(0);
}

TEST(futex, killed_thread_unblocks) {
    g_kill_val = 0;
    g_kill_entered.store_relaxed(0);
    g_kill_task = nullptr;

    rc::strong_ref<sched::task> pin;
    RUN_ELEVATED({
        g_kill_task = sched::create_kernel_task(
            kill_waiter_fn, nullptr, "ftx_kill");
        ASSERT_NOT_NULL(g_kill_task);
        pin = sched::task_ref(g_kill_task);
        sched::enqueue(g_kill_task);
    });

    ASSERT_TRUE(spin_wait(g_kill_entered));
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
static uint32_t g_wall_val = 0;
static sync::atomic<uint32_t> g_wall_ready;
static sync::atomic<uint32_t> g_wall_woken;

static void wake_all_waiter_fn(void*) {
    g_wall_ready.fetch_add_acq_rel(1);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_wall_val), 0, 0);
    });
    g_wall_woken.fetch_add_acq_rel(1);
    sched::exit(0);
}

TEST(futex, wake_all_wakes_everyone) {
    g_wall_val = 0;
    g_wall_ready.store_relaxed(0);
    g_wall_woken.store_relaxed(0);

    RUN_ELEVATED({
        for (uint32_t i = 0; i < WALL_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                wake_all_waiter_fn, nullptr, "ftx_wall");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(g_wall_ready, WALL_TASKS));
    brief_delay();

    int32_t woken = 0;
    RUN_ELEVATED({
        woken = sync::futex_wake_all(
            reinterpret_cast<uintptr_t>(&g_wall_val));
    });
    EXPECT_EQ(woken, static_cast<int32_t>(WALL_TASKS));
    EXPECT_TRUE(spin_wait_ge(g_wall_woken, WALL_TASKS));
}

// --- different addresses are independent ---

static uint32_t g_ind_val_a = 0;
static uint32_t g_ind_val_b = 0;
static sync::atomic<uint32_t> g_ind_ready_a;
static sync::atomic<uint32_t> g_ind_ready_b;
static sync::atomic<uint32_t> g_ind_woken_a;
static sync::atomic<uint32_t> g_ind_woken_b;

static void ind_waiter_a_fn(void*) {
    g_ind_ready_a.store_release(1);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_ind_val_a), 0, 0);
    });
    g_ind_woken_a.store_release(1);
    sched::exit(0);
}

static void ind_waiter_b_fn(void*) {
    g_ind_ready_b.store_release(1);
    RUN_ELEVATED({
        sync::futex_wait(
            reinterpret_cast<uintptr_t>(&g_ind_val_b), 0, 0);
    });
    g_ind_woken_b.store_release(1);
    sched::exit(0);
}

TEST(futex, different_addresses_independent) {
    g_ind_val_a = 0;
    g_ind_val_b = 0;
    g_ind_ready_a.store_relaxed(0);
    g_ind_ready_b.store_relaxed(0);
    g_ind_woken_a.store_relaxed(0);
    g_ind_woken_b.store_relaxed(0);

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

    ASSERT_TRUE(spin_wait(g_ind_ready_a));
    ASSERT_TRUE(spin_wait(g_ind_ready_b));
    brief_delay();

    // Wake only address A
    RUN_ELEVATED({
        sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_ind_val_a), 1);
    });

    ASSERT_TRUE(spin_wait(g_ind_woken_a));
    brief_delay();
    EXPECT_EQ(g_ind_woken_b.load_acquire(), 0u);

    // Now wake B
    RUN_ELEVATED({
        sync::futex_wake(
            reinterpret_cast<uintptr_t>(&g_ind_val_b), 1);
    });

    EXPECT_TRUE(spin_wait(g_ind_woken_b));
}

static uint32_t g_rq_start = 0;
static uint32_t g_rq_hold = 0;
static uint32_t g_rq_dest = 0;
static sync::atomic<uint32_t> g_rq_ready;
static sync::atomic<uint32_t> g_rq_woken;

static uintptr_t word_addr(uint32_t& word) {
    return reinterpret_cast<uintptr_t>(&word);
}

static int32_t requeue(uint32_t& from, uint32_t& to, uint32_t nr_wake, uint32_t nr_requeue,
                       const uint32_t* expected = nullptr) {
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = sync::futex_requeue(word_addr(from), word_addr(to), nr_wake, nr_requeue, expected);
    });
    return rc;
}

static int32_t wake(uint32_t& word, uint32_t count) {
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = sync::futex_wake(word_addr(word), count);
    });
    return rc;
}

// Requeues `count` waiters from `start` to `hold`, which proves each is queued before a test acts on it
static bool gather_waiters(uint32_t& start, uint32_t& hold, uint32_t count) {
    uint32_t gathered = 0;
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (gathered < count && clock::now_ns() < deadline) {
        int32_t moved = requeue(start, hold, 0, count - gathered);
        gathered += moved > 0 ? static_cast<uint32_t>(moved) : 0;
    }

    return gathered == count;
}

static void requeue_waiter_fn(void*) {
    g_rq_ready.fetch_add_acq_rel(1);
    RUN_ELEVATED({
        sync::futex_wait(word_addr(g_rq_start), 0, 0);
    });
    g_rq_woken.fetch_add_acq_rel(1);
    sched::exit(0);
}

static bool park_waiters(uint32_t count) {
    g_rq_ready.store_relaxed(0);
    g_rq_woken.store_relaxed(0);

    bool created = true;
    RUN_ELEVATED({
        for (uint32_t i = 0; i < count && created; i++) {
            sched::task* t = sched::create_kernel_task(requeue_waiter_fn, nullptr, "ftx_rq");
            created = t != nullptr;
            if (created) {
                sched::enqueue(t);
            }
        }
    });

    return created && spin_wait_ge(g_rq_ready, count) && gather_waiters(g_rq_start, g_rq_hold, count);
}

TEST(futex, requeue_moves_waiters_without_waking_them) {
    ASSERT_TRUE(park_waiters(3));

    EXPECT_EQ(requeue(g_rq_hold, g_rq_dest, 0, 3), 3);
    EXPECT_EQ(wake(g_rq_hold, 3), 0);
    brief_delay();
    EXPECT_EQ(g_rq_woken.load_acquire(), 0u);

    EXPECT_EQ(wake(g_rq_dest, 3), 3);
    EXPECT_TRUE(spin_wait_ge(g_rq_woken, 3));
}

TEST(futex, requeue_wakes_the_first_waiters_then_moves_the_next) {
    ASSERT_TRUE(park_waiters(3));

    EXPECT_EQ(requeue(g_rq_hold, g_rq_dest, 1, 1), 2);
    ASSERT_TRUE(spin_wait_ge(g_rq_woken, 1));
    brief_delay();
    EXPECT_EQ(g_rq_woken.load_acquire(), 1u);

    EXPECT_EQ(wake(g_rq_hold, 3), 1);
    EXPECT_EQ(wake(g_rq_dest, 3), 1);
    EXPECT_TRUE(spin_wait_ge(g_rq_woken, 3));
}

constexpr uint32_t MANY_WAITERS = 20;
constexpr uint32_t MANY_TO_WAKE = 18; // More than one wake batch holds
constexpr uint32_t MANY_TO_MOVE = MANY_WAITERS - MANY_TO_WAKE;

TEST(futex, requeue_wakes_more_waiters_than_one_batch_holds) {
    ASSERT_TRUE(park_waiters(MANY_WAITERS));

    EXPECT_EQ(requeue(g_rq_hold, g_rq_dest, MANY_TO_WAKE, MANY_TO_MOVE), static_cast<int32_t>(MANY_WAITERS));
    ASSERT_TRUE(spin_wait_ge(g_rq_woken, MANY_TO_WAKE));
    brief_delay();
    EXPECT_EQ(g_rq_woken.load_acquire(), MANY_TO_WAKE);

    EXPECT_EQ(wake(g_rq_hold, MANY_WAITERS), 0);
    EXPECT_EQ(wake(g_rq_dest, MANY_WAITERS), static_cast<int32_t>(MANY_TO_MOVE));
    EXPECT_TRUE(spin_wait_ge(g_rq_woken, MANY_WAITERS));
}

TEST(futex, compare_requeue_moves_nothing_once_the_word_changed) {
    ASSERT_TRUE(park_waiters(1));

    uint32_t stale = 1;
    EXPECT_EQ(requeue(g_rq_hold, g_rq_dest, 0, 1, &stale), syscall::EAGAIN);
    EXPECT_EQ(wake(g_rq_dest, 1), 0);

    uint32_t current = 0;
    EXPECT_EQ(requeue(g_rq_hold, g_rq_dest, 0, 1, &current), 1);
    EXPECT_EQ(wake(g_rq_dest, 1), 1);
    EXPECT_TRUE(spin_wait(g_rq_woken));
}

TEST(futex, requeue_onto_the_same_word_keeps_waiters_queued) {
    ASSERT_TRUE(park_waiters(2));

    EXPECT_EQ(requeue(g_rq_hold, g_rq_hold, 0, 2), 2);
    brief_delay();
    EXPECT_EQ(g_rq_woken.load_acquire(), 0u);

    EXPECT_EQ(wake(g_rq_hold, 2), 2);
    EXPECT_TRUE(spin_wait_ge(g_rq_woken, 2));
}

TEST(futex, requeue_rejects_misaligned_words) {
    uint32_t pair[2] = {};
    uintptr_t misaligned = reinterpret_cast<uintptr_t>(pair) + 1;

    int32_t rc = 0;
    RUN_ELEVATED({
        rc = sync::futex_requeue(misaligned, word_addr(g_rq_dest), 0, 1, nullptr);
    });
    EXPECT_EQ(rc, syscall::EINVAL);
}

constexpr uint64_t REQUEUED_WAIT_TIMEOUT_NS = 500000000ULL; // 500ms

static uint32_t g_rqt_start = 0;
static uint32_t g_rqt_dest = 0;
static sync::atomic<uint32_t> g_rqt_ready;
static sync::atomic<uint32_t> g_rqt_done;
static sync::atomic<int32_t> g_rqt_rc;

static void requeue_timeout_waiter_fn(void*) {
    g_rqt_ready.store_release(1);
    RUN_ELEVATED({
        g_rqt_rc.store_relaxed(sync::futex_wait(word_addr(g_rqt_start), 0, REQUEUED_WAIT_TIMEOUT_NS));
    });
    g_rqt_done.store_release(1);
    sched::exit(0);
}

TEST(futex, requeued_waiter_times_out_from_the_queue_it_was_moved_to) {
    g_rqt_ready.store_relaxed(0);
    g_rqt_done.store_relaxed(0);
    g_rqt_rc.store_relaxed(0);

    bool created = false;
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(requeue_timeout_waiter_fn, nullptr, "ftx_rqt");
        if (t) {
            sched::enqueue(t);
            created = true;
        }
    });
    ASSERT_TRUE(created);
    ASSERT_TRUE(spin_wait(g_rqt_ready));
    ASSERT_TRUE(gather_waiters(g_rqt_start, g_rqt_dest, 1));

    ASSERT_TRUE(spin_wait(g_rqt_done));
    EXPECT_EQ(g_rqt_rc.load_acquire(), syscall::ETIMEDOUT);
    EXPECT_EQ(wake(g_rqt_dest, 1), 0);
}

constexpr uint32_t BITSET_A       = 0x1;
constexpr uint32_t BITSET_B       = 0x2;
constexpr uint32_t BITSET_NEITHER = 0x4;

static uint32_t g_bs_start = 0;
static uint32_t g_bs_hold = 0;
static sync::atomic<uint32_t> g_bs_ready;
static sync::atomic<uint32_t> g_bs_woken; // Bitsets of the waiters woken so far

static void bitset_waiter_fn(void* arg) {
    uint32_t bitset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    g_bs_ready.fetch_add_acq_rel(1);
    RUN_ELEVATED({
        sync::futex_wait_until(word_addr(g_bs_start), 0, 0, bitset);
    });
    g_bs_woken.fetch_or_acq_rel(bitset);
    sched::exit(0);
}

static int32_t wake_bitset(uint32_t& word, uint32_t count, uint32_t bitset) {
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = sync::futex_wake_bitset(word_addr(word), count, bitset);
    });
    return rc;
}

static bool start_bitset_waiter(uint32_t bitset) {
    bool created = false;
    RUN_ELEVATED({
        void* arg = reinterpret_cast<void*>(static_cast<uintptr_t>(bitset));
        sched::task* t = sched::create_kernel_task(bitset_waiter_fn, arg, "ftx_bs");
        if (t) {
            sched::enqueue(t);
            created = true;
        }
    });
    return created;
}

TEST(futex, bitset_wakes_reach_only_overlapping_waiters) {
    g_bs_ready.store_relaxed(0);
    g_bs_woken.store_relaxed(0);

    ASSERT_TRUE(start_bitset_waiter(BITSET_A));
    ASSERT_TRUE(start_bitset_waiter(BITSET_B));
    ASSERT_TRUE(spin_wait_ge(g_bs_ready, 2));
    ASSERT_TRUE(gather_waiters(g_bs_start, g_bs_hold, 2));

    EXPECT_EQ(wake_bitset(g_bs_hold, 2, BITSET_NEITHER), 0);
    EXPECT_EQ(wake_bitset(g_bs_hold, 2, BITSET_B), 1);
    ASSERT_TRUE(spin_wait_ge(g_bs_woken, BITSET_B));
    brief_delay();
    EXPECT_EQ(g_bs_woken.load_acquire(), BITSET_B);

    EXPECT_EQ(wake(g_bs_hold, 2), 1);
    ASSERT_TRUE(spin_wait_ge(g_bs_woken, BITSET_A | BITSET_B));
    EXPECT_EQ(g_bs_woken.load_acquire(), BITSET_A | BITSET_B);
}

TEST(futex, zero_bitsets_are_refused) {
    int32_t wait_rc = 0;
    int32_t wake_rc = 0;
    RUN_ELEVATED({
        wait_rc = sync::futex_wait_until(word_addr(g_bs_hold), 0, 0, 0);
        wake_rc = sync::futex_wake_bitset(word_addr(g_bs_hold), 1, 0);
    });
    EXPECT_EQ(wait_rc, syscall::EINVAL);
    EXPECT_EQ(wake_rc, syscall::EINVAL);
}

constexpr uint64_t DEADLINE_WAIT_NS   = 50000000ULL; // 50ms
constexpr uint64_t PASSED_DEADLINE_NS = 1;

static uint32_t g_dl_word = 0;
static sync::atomic<int32_t> g_dl_rc;
static sync::atomic<uint64_t> g_dl_elapsed;
static sync::atomic<uint32_t> g_dl_done;

static void deadline_waiter_fn(void* deadline_passed) {
    uint64_t start = clock::now_ns();
    uint64_t deadline = deadline_passed ? PASSED_DEADLINE_NS : start + DEADLINE_WAIT_NS;
    RUN_ELEVATED({
        g_dl_rc.store_relaxed(sync::futex_wait_until(word_addr(g_dl_word), 0, deadline, sync::FUTEX_BITSET_ANY));
    });
    g_dl_elapsed.store_release(clock::now_ns() - start);
    g_dl_done.store_release(1);
    sched::exit(0);
}

static bool run_deadline_waiter(bool deadline_passed) {
    g_dl_rc.store_relaxed(0);
    g_dl_elapsed.store_relaxed(0);
    g_dl_done.store_relaxed(0);

    bool created = false;
    RUN_ELEVATED({
        void* arg = reinterpret_cast<void*>(static_cast<uintptr_t>(deadline_passed));
        sched::task* t = sched::create_kernel_task(deadline_waiter_fn, arg, "ftx_dl");
        if (t) {
            sched::enqueue(t);
            created = true;
        }
    });

    return created && spin_wait(g_dl_done);
}

TEST(futex, wait_until_times_out_at_its_deadline) {
    ASSERT_TRUE(run_deadline_waiter(false));
    EXPECT_EQ(g_dl_rc.load_acquire(), syscall::ETIMEDOUT);
    EXPECT_GE(g_dl_elapsed.load_acquire(), DEADLINE_WAIT_NS);
}

TEST(futex, wait_until_a_passed_deadline_times_out_at_once) {
    ASSERT_TRUE(run_deadline_waiter(true));
    EXPECT_EQ(g_dl_rc.load_acquire(), syscall::ETIMEDOUT);
    EXPECT_LT(g_dl_elapsed.load_acquire(), DEADLINE_WAIT_NS);
}
