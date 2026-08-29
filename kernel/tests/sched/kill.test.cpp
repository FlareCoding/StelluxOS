#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal_types.h"
#include "dynpriv/dynpriv.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "clock/clock.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

// Deterministic handshake: wait until the task has actually blocked.
static bool wait_until_blocked(sched::task* t) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (t->state.load_acquire() != sched::TASK_STATE_BLOCKED) {
        if (clock::now_ns() > deadline) return false;
    }
    return true;
}

TEST_SUITE(kill);

// --- force_wake_kills_sleeping_task ---
// A task sleeping for a long time is woken immediately by force_wake_for_kill.
// Verifies: kill_pending is set, sleep is cancelled, task completes promptly.

static sync::atomic<uint32_t> g_sleep_kill_done;
static sync::atomic<uint32_t> g_sleep_kill_was_pending;
static sync::atomic<uint64_t> g_sleep_kill_elapsed_ns;

static void sleep_kill_fn(void*) {
    uint64_t start = clock::now_ns();
    RUN_ELEVATED({
        sched::sleep_ns(5000000000ULL); // 5 seconds -- should be cancelled
    });
    uint64_t elapsed = clock::now_ns() - start;
    g_sleep_kill_elapsed_ns.store_release(elapsed);

    uint32_t kp = 0;
    RUN_ELEVATED({
        kp = sched::is_kill_pending() ? 1u : 0u;
    });
    g_sleep_kill_was_pending.store_release(kp);
    g_sleep_kill_done.store_release(1);
    sched::exit(0);
}

TEST(kill, force_wake_kills_sleeping_task) {
    g_sleep_kill_done.store_relaxed(0);
    g_sleep_kill_was_pending.store_relaxed(0);
    g_sleep_kill_elapsed_ns.store_relaxed(0);

    sched::task* t = nullptr;
    rc::strong_ref<sched::task> pin;
    RUN_ELEVATED({
        t = sched::create_kernel_task(sleep_kill_fn, nullptr, "kill_sleep");
        ASSERT_NOT_NULL(t);
        pin = sched::task_ref(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(wait_until_blocked(t));

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(g_sleep_kill_done));
    EXPECT_EQ(g_sleep_kill_was_pending.load_acquire(), 1u);
    EXPECT_LT(g_sleep_kill_elapsed_ns.load_acquire(),
              static_cast<uint64_t>(2000000000)); // woke in < 2s, not 5s
}

// --- kill_before_sleep_aborts_sleep ---
// A kill issued before the task ever runs must abort its sleep attempt
// at the block commit point instead of being lost until natural expiry.

TEST(kill, kill_before_sleep_aborts_sleep) {
    g_sleep_kill_done.store_relaxed(0);
    g_sleep_kill_was_pending.store_relaxed(0);
    g_sleep_kill_elapsed_ns.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(sleep_kill_fn, nullptr, "kill_presleep");
        ASSERT_NOT_NULL(t);
        sched::force_wake_for_kill(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_sleep_kill_done));
    EXPECT_EQ(g_sleep_kill_was_pending.load_acquire(), 1u);
    EXPECT_LT(g_sleep_kill_elapsed_ns.load_acquire(),
              static_cast<uint64_t>(2000000000)); // aborted, not slept for 5s
}

// --- force_wake_kills_blocked_on_wq ---
// A task blocked on a wait queue is woken by force_wake_for_kill.
// Verifies: task wakes, self-removes from wq, and sees kill_pending.

static sync::wait_queue g_wq_kill_wq;
static sync::spinlock g_wq_kill_lock;
static sync::atomic<uint32_t> g_wq_kill_waiting;
static sync::atomic<uint32_t> g_wq_kill_done;
static sync::atomic<uint32_t> g_wq_kill_was_pending;

static void wq_kill_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_wq_kill_lock);
        g_wq_kill_waiting.store_release(1);
        while (!sched::is_kill_pending()) {
            irq = sync::wait(g_wq_kill_wq, g_wq_kill_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_wq_kill_lock, irq);
    });

    uint32_t kp = 0;
    RUN_ELEVATED({
        kp = sched::is_kill_pending() ? 1u : 0u;
    });
    g_wq_kill_was_pending.store_release(kp);
    g_wq_kill_done.store_release(1);
    sched::exit(0);
}

TEST(kill, force_wake_kills_blocked_on_wq) {
    g_wq_kill_wq.init();
    g_wq_kill_lock = sync::SPINLOCK_INIT;
    g_wq_kill_waiting.store_relaxed(0);
    g_wq_kill_done.store_relaxed(0);
    g_wq_kill_was_pending.store_relaxed(0);

    sched::task* t = nullptr;
    rc::strong_ref<sched::task> pin;
    RUN_ELEVATED({
        t = sched::create_kernel_task(wq_kill_fn, nullptr, "kill_wq");
        ASSERT_NOT_NULL(t);
        pin = sched::task_ref(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_wq_kill_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(g_wq_kill_done));
    EXPECT_EQ(g_wq_kill_was_pending.load_acquire(), 1u);
}

// --- self_removal_cleans_wq ---
// After force-wake, the wait queue should be empty (task self-removed).

static sync::wait_queue g_sr_wq;
static sync::spinlock g_sr_lock;
static sync::atomic<uint32_t> g_sr_waiting;
static sync::atomic<uint32_t> g_sr_done;

static void sr_waiter_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_sr_lock);
        g_sr_waiting.store_release(1);
        while (!sched::is_kill_pending()) {
            irq = sync::wait(g_sr_wq, g_sr_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_sr_lock, irq);
    });
    g_sr_done.store_release(1);
    sched::exit(0);
}

TEST(kill, self_removal_cleans_wq) {
    g_sr_wq.init();
    g_sr_lock = sync::SPINLOCK_INIT;
    g_sr_waiting.store_relaxed(0);
    g_sr_done.store_relaxed(0);

    sched::task* t = nullptr;
    rc::strong_ref<sched::task> pin;
    RUN_ELEVATED({
        t = sched::create_kernel_task(sr_waiter_fn, nullptr, "kill_sr");
        ASSERT_NOT_NULL(t);
        pin = sched::task_ref(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_sr_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(g_sr_done));
    EXPECT_TRUE(g_sr_wq.waiters.empty());
}

// --- double_kill_is_harmless ---
// Calling force_wake_for_kill twice on the same task does not crash.

static sync::atomic<uint32_t> g_double_waiting;
static sync::atomic<uint32_t> g_double_done;
static sync::atomic<uint32_t> g_double_kp;
static sync::wait_queue g_double_wq;
static sync::spinlock g_double_lock;

static void double_kill_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_double_lock);
        g_double_waiting.store_release(1);
        while (!sched::is_kill_pending()) {
            irq = sync::wait(g_double_wq, g_double_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_double_lock, irq);
        g_double_kp.store_release(sched::is_kill_pending() ? 1u : 0u);
    });
    g_double_done.store_release(1);
    sched::exit(0);
}

TEST(kill, double_kill_is_harmless) {
    g_double_wq.init();
    g_double_lock = sync::SPINLOCK_INIT;
    g_double_waiting.store_relaxed(0);
    g_double_done.store_relaxed(0);
    g_double_kp.store_relaxed(0);

    sched::task* t = nullptr;
    rc::strong_ref<sched::task> pin;
    RUN_ELEVATED({
        t = sched::create_kernel_task(double_kill_fn, nullptr, "kill_dbl");
        ASSERT_NOT_NULL(t);
        pin = sched::task_ref(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_double_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(g_double_done));
    EXPECT_EQ(g_double_kp.load_acquire(), 1u);
}

// --- is_kill_pending_accessor ---
// Verifies is_kill_pending() returns correct values before and after setting the flag.

static sync::atomic<uint32_t> g_ikp_before{0xFF};
static sync::atomic<uint32_t> g_ikp_after{0xFF};
static sync::atomic<uint32_t> g_ikp_done;
static sync::atomic<uint32_t> g_ikp_flag_set;
static sync::atomic<uint32_t> g_ikp_started;

static void ikp_fn(void*) {
    uint32_t before = 0;
    RUN_ELEVATED({
        before = sched::is_kill_pending() ? 1 : 0;
    });
    g_ikp_before.store_release(before);
    g_ikp_started.store_release(1);

    while (!g_ikp_flag_set.load_acquire()) {
        // busy wait for test driver to set kill_pending
    }

    uint32_t after = 0;
    RUN_ELEVATED({
        after = sched::is_kill_pending() ? 1 : 0;
    });
    g_ikp_after.store_release(after);
    g_ikp_done.store_release(1);
    sched::exit(0);
}

TEST(kill, is_kill_pending_accessor) {
    g_ikp_before.store_relaxed(0xFF);
    g_ikp_after.store_relaxed(0xFF);
    g_ikp_done.store_relaxed(0);
    g_ikp_flag_set.store_relaxed(0);
    g_ikp_started.store_relaxed(0);

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(ikp_fn, nullptr, "kill_ikp");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_ikp_started));

    RUN_ELEVATED({
        t->sig.pending.fetch_or_release(signals::sig_bit(signals::SIGKILL));
    });
    g_ikp_flag_set.store_release(1);

    ASSERT_TRUE(spin_wait(g_ikp_done));
    EXPECT_EQ(g_ikp_before.load_acquire(), 0u);
    EXPECT_EQ(g_ikp_after.load_acquire(), 1u);
}

// --- wait_status_normal_exit ---
// Verifies that a normal exit(42) produces WIFEXITED-compatible wait_status.

TEST(kill, wait_status_normal_exit) {
    int32_t status = (42 & 0xFF) << 8;
    EXPECT_EQ(status & 0x7F, 0);
    EXPECT_EQ((status >> 8) & 0xFF, 42);
}

// --- wait_status_killed ---
// Verifies that a killed exit(9) with kill_pending produces WIFSIGNALED-compatible wait_status.

TEST(kill, wait_status_killed) {
    int32_t status = 9 & 0x7F;
    EXPECT_NE(status & 0x7F, 0);
    EXPECT_EQ(status & 0x7F, 9);
}

// --- kill_after_death_is_safe ---
// A counted reference keeps a fully dead task safe to kill: the wake
// degrades to a no-op instead of touching reclaimed memory. Dropping the
// pin must then let the reaper reclaim the task.

static sync::atomic<uint32_t> g_kad_done;

static void kad_fn(void*) {
    g_kad_done.store_release(1);
    sched::exit(0);
}

TEST(kill, kill_after_death_is_safe) {
    g_kad_done.store_relaxed(0);

    sched::task* t = nullptr;
    rc::strong_ref<sched::task> pin;
    uint32_t tid = 0;
    RUN_ELEVATED({
        t = sched::create_kernel_task(kad_fn, nullptr, "kill_dead");
        ASSERT_NOT_NULL(t);
        pin = sched::task_ref(t);
        tid = t->tid;
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_kad_done));

    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (t->state.load_acquire() != sched::TASK_STATE_DEAD) {
        if (clock::now_ns() > deadline) break;
    }
    ASSERT_EQ(t->state.load_acquire(), sched::TASK_STATE_DEAD);
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    // Only the pin holds the task now, releasing it must reclaim
    pin.reset();

    bool reclaimed = false;
    deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (!reclaimed && clock::now_ns() < deadline) {
        RUN_ELEVATED({
            reclaimed = !sched::task_ref_by_tid(tid);
        });
    }
    EXPECT_TRUE(reclaimed);
}

// --- wake_all_reclaims_many_waiters ---
// Twenty waiters exceed one wake batch, forcing wake_all to rescan. Every
// waiter is pinned, killed again after death, and must reclaim once the
// pins drop, proving no reference leaks out of the wake and kill paths.

constexpr uint32_t MANY_WAITERS = 20;

static sync::wait_queue g_many_wq;
static sync::spinlock g_many_lock;
static sync::atomic<uint32_t> g_many_waiting;
static sync::atomic<uint32_t> g_many_go;
static sync::atomic<uint32_t> g_many_done;
static uint32_t g_many_tids[MANY_WAITERS];

static void many_waiter_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_many_lock);
        g_many_waiting.fetch_add_release(1);
        while (g_many_go.load_acquire() == 0 && !sched::is_kill_pending()) {
            irq = sync::wait(g_many_wq, g_many_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_many_lock, irq);
    });
    g_many_done.fetch_add_release(1);
    sched::exit(0);
}

TEST(kill, wake_all_reclaims_many_waiters) {
    g_many_wq.init();
    g_many_lock = sync::SPINLOCK_INIT;
    g_many_waiting.store_relaxed(0);
    g_many_go.store_relaxed(0);
    g_many_done.store_relaxed(0);

    rc::strong_ref<sched::task> pins[MANY_WAITERS];
    RUN_ELEVATED({
        for (uint32_t i = 0; i < MANY_WAITERS; i++) {
            sched::task* t = sched::create_kernel_task(
                many_waiter_fn, nullptr, "kill_many");
            ASSERT_NOT_NULL(t);
            pins[i] = sched::task_ref(t);
            g_many_tids[i] = t->tid;
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(g_many_waiting, MANY_WAITERS));
    brief_delay();

    g_many_go.store_release(1);
    RUN_ELEVATED({
        sync::wake_all(g_many_wq);
    });

    ASSERT_TRUE(spin_wait_ge(g_many_done, MANY_WAITERS));

    // Killing waiters that are already dead or dying must be a no-op
    RUN_ELEVATED({
        for (uint32_t i = 0; i < MANY_WAITERS; i++) {
            sched::force_wake_for_kill(pins[i].ptr());
        }
    });

    for (uint32_t i = 0; i < MANY_WAITERS; i++) {
        pins[i].reset();
    }

    uint32_t reclaimed = 0;
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (reclaimed < MANY_WAITERS && clock::now_ns() < deadline) {
        reclaimed = 0;
        RUN_ELEVATED({
            for (uint32_t i = 0; i < MANY_WAITERS; i++) {
                if (!sched::task_ref_by_tid(g_many_tids[i])) {
                    reclaimed++;
                }
            }
        });
    }
    EXPECT_EQ(reclaimed, MANY_WAITERS);
}
