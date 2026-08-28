#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "sync/poll.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(poll);

// basic_subscribe_and_trigger
// One task subscribes to a single wait queue, another fires it.

static sync::wait_queue g_basic_wq;
static sync::atomic<uint32_t> g_basic_waiting;
static sync::atomic<uint32_t> g_basic_result;
static sync::atomic<uint32_t> g_basic_done;

static void basic_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_basic_wq);

        g_basic_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, 0);
        g_basic_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_basic_done.store_release(1);
    sched::exit(0);
}

TEST(poll, basic_subscribe_and_trigger) {
    g_basic_wq.init();
    g_basic_waiting.store_relaxed(0);
    g_basic_result.store_relaxed(0);
    g_basic_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(basic_poll_fn, nullptr, "poll_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_basic_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_basic_wq);
    });

    ASSERT_TRUE(spin_wait(g_basic_done));
    EXPECT_EQ(g_basic_result.load_acquire(), 1u);
}

// multi_source_first_fires
// Subscribe to 3 wait queues, fire the first one.

static sync::wait_queue g_multi_wq[3];
static sync::atomic<uint32_t> g_multi_first_waiting;
static sync::atomic<uint32_t> g_multi_first_result;
static sync::atomic<uint32_t> g_multi_first_done;

static void multi_first_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_multi_wq[i]);
        }

        g_multi_first_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, 0);
        g_multi_first_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_multi_first_done.store_release(1);
    sched::exit(0);
}

TEST(poll, multi_source_first_fires) {
    for (int i = 0; i < 3; i++) g_multi_wq[i].init();
    g_multi_first_waiting.store_relaxed(0);
    g_multi_first_result.store_relaxed(0);
    g_multi_first_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(multi_first_fn, nullptr, "poll_mf");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_multi_first_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_multi_wq[0]);
    });

    ASSERT_TRUE(spin_wait(g_multi_first_done));
    EXPECT_EQ(g_multi_first_result.load_acquire(), 1u);
}

// multi_source_last_fires
// Subscribe to 3 wait queues, fire the last one.

static sync::atomic<uint32_t> g_multi_last_waiting;
static sync::atomic<uint32_t> g_multi_last_result;
static sync::atomic<uint32_t> g_multi_last_done;

static void multi_last_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_multi_wq[i]);
        }

        g_multi_last_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, 0);
        g_multi_last_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_multi_last_done.store_release(1);
    sched::exit(0);
}

TEST(poll, multi_source_last_fires) {
    for (int i = 0; i < 3; i++) g_multi_wq[i].init();
    g_multi_last_waiting.store_relaxed(0);
    g_multi_last_result.store_relaxed(0);
    g_multi_last_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(multi_last_fn, nullptr, "poll_ml");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_multi_last_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_multi_wq[2]);
    });

    ASSERT_TRUE(spin_wait(g_multi_last_done));
    EXPECT_EQ(g_multi_last_result.load_acquire(), 1u);
}

// multi_source_all_fire
// Subscribe to 3 wait queues, fire all of them. Task should wake once.

static sync::atomic<uint32_t> g_multi_all_waiting;
static sync::atomic<uint32_t> g_multi_all_result;
static sync::atomic<uint32_t> g_multi_all_done;

static void multi_all_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_multi_wq[i]);
        }

        g_multi_all_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, 0);
        g_multi_all_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_multi_all_done.store_release(1);
    sched::exit(0);
}

TEST(poll, multi_source_all_fire) {
    for (int i = 0; i < 3; i++) g_multi_wq[i].init();
    g_multi_all_waiting.store_relaxed(0);
    g_multi_all_result.store_relaxed(0);
    g_multi_all_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(multi_all_fn, nullptr, "poll_ma");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_multi_all_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_multi_wq[0]);
        sync::wake_one(g_multi_wq[1]);
        sync::wake_one(g_multi_wq[2]);
    });

    ASSERT_TRUE(spin_wait(g_multi_all_done));
    EXPECT_EQ(g_multi_all_result.load_acquire(), 1u);
}

// timeout_no_trigger
// Subscribe but never fire. Wait with timeout. Verify returns false.

static sync::atomic<uint32_t> g_timeout_result;
static sync::atomic<uint32_t> g_timeout_done;

static void timeout_fn(void*) {
    RUN_ELEVATED({
        sync::wait_queue wq;
        wq.init();
        sync::poll_table pt;
        pt.init(sched::current());
        sync::poll_subscribe(pt, wq);

        bool triggered = sync::poll_wait(pt, 50000000ULL); // 50ms
        g_timeout_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_timeout_done.store_release(1);
    sched::exit(0);
}

TEST(poll, timeout_no_trigger) {
    g_timeout_result.store_relaxed(0);
    g_timeout_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(timeout_fn, nullptr, "poll_to");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_timeout_done));
    EXPECT_EQ(g_timeout_result.load_acquire(), 0u);
}

// immediate_trigger_no_block
// Fire the source before poll_wait, should return immediately.

static sync::atomic<uint32_t> g_imm_result;
static sync::atomic<uint32_t> g_imm_done;

static void immediate_fn(void*) {
    RUN_ELEVATED({
        sync::wait_queue wq;
        wq.init();
        sync::poll_table pt;
        pt.init(sched::current());
        sync::poll_subscribe(pt, wq);

        // Fire the source before waiting
        sync::wake_one(wq);

        bool triggered = sync::poll_wait(pt, 0);
        g_imm_result.store_release(triggered ? 1 : 0);

        sync::poll_cleanup(pt);
    });
    g_imm_done.store_release(1);
    sched::exit(0);
}

TEST(poll, immediate_trigger_no_block) {
    g_imm_result.store_relaxed(0);
    g_imm_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(immediate_fn, nullptr, "poll_imm");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_imm_done));
    EXPECT_EQ(g_imm_result.load_acquire(), 1u);
}

// cleanup_removes_all_observers
// Subscribe to 3 wqs, cleanup, verify observer lists are empty.

static sync::atomic<uint32_t> g_cleanup_done;

static void cleanup_fn(void*) {
    RUN_ELEVATED({
        sync::wait_queue wqs[3];
        for (int i = 0; i < 3; i++) wqs[i].init();

        sync::poll_table pt;
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, wqs[i]);
        }

        sync::poll_cleanup(pt);

        // Verify all observer lists are empty
        bool all_empty = true;
        for (int i = 0; i < 3; i++) {
            sync::irq_state irq = sync::spin_lock_irqsave(wqs[i].lock);
            if (!wqs[i].observers.empty()) all_empty = false;
            sync::spin_unlock_irqrestore(wqs[i].lock, irq);
        }
        if (!all_empty) {
            g_cleanup_done.store_release(2);
        } else {
            g_cleanup_done.store_release(1);
        }
    });
    sched::exit(0);
}

TEST(poll, cleanup_removes_all_observers) {
    g_cleanup_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(cleanup_fn, nullptr, "poll_cl");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_cleanup_done));
    EXPECT_EQ(g_cleanup_done.load_acquire(), 1u);
}

// kill_pending_wakes_poll
// Task in poll_wait, force_wake_for_kill wakes it.

static sync::wait_queue g_kill_wq;
static sync::atomic<uint32_t> g_kill_waiting;
static sync::atomic<uint32_t> g_kill_result;
static sync::atomic<uint32_t> g_kill_kp;
static sync::atomic<uint32_t> g_kill_sched_linked;
static sync::atomic<uint32_t> g_kill_state_after_wait;
static sync::atomic<uint32_t> g_kill_done;

static void kill_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_kill_wq);

        g_kill_waiting.store_release(1);
        bool triggered = sync::poll_wait(pt, 0);
        g_kill_result.store_release(triggered ? 1 : 0);
        g_kill_kp.store_release(sched::is_kill_pending() ? 1 : 0);
        g_kill_sched_linked.store_release(sched::current()->sched_link.is_linked() ? 1 : 0);
        g_kill_state_after_wait.store_release(sched::current()->state.load_relaxed());

        sync::poll_cleanup(pt);
    });
    g_kill_done.store_release(1);
    sched::exit(0);
}

TEST(poll, kill_pending_wakes_poll) {
    g_kill_wq.init();
    g_kill_waiting.store_relaxed(0);
    g_kill_result.store_relaxed(0);
    g_kill_kp.store_relaxed(0);
    g_kill_sched_linked.store_relaxed(0);
    g_kill_state_after_wait.store_relaxed(0);
    g_kill_done.store_relaxed(0);

    sched::task* t = nullptr;
    rc::strong_ref<sched::task> pin;
    RUN_ELEVATED({
        t = sched::create_kernel_task(kill_poll_fn, nullptr, "poll_kill");
        ASSERT_NOT_NULL(t);
        pin = sched::task_ref(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(g_kill_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(g_kill_done));
    // Source was not fired, so triggered should be 0
    EXPECT_EQ(g_kill_result.load_acquire(), 0u);
    EXPECT_EQ(g_kill_kp.load_acquire(), 1u);
    EXPECT_EQ(g_kill_sched_linked.load_acquire(), 0u);
    EXPECT_EQ(g_kill_state_after_wait.load_acquire(),
              sched::TASK_STATE_RUNNING);
}

// repeated_poll_cycles
// Subscribe/wait/cleanup 10 times on the same wqs.

static sync::wait_queue g_repeat_wq;
static sync::spinlock g_repeat_lock;
static sync::atomic<uint32_t> g_repeat_counter;
static sync::atomic<uint32_t> g_repeat_waiting;
static sync::atomic<uint32_t> g_repeat_progress;

constexpr uint32_t REPEAT_CYCLES = 10;

static void repeat_poll_fn(void*) {
    for (uint32_t iter = 0; iter < REPEAT_CYCLES; iter++) {
        RUN_ELEVATED({
            sync::poll_table pt;
            pt.init(sched::current());
            sync::poll_subscribe(pt, g_repeat_wq);

            g_repeat_waiting.store_release(iter + 1);
            sync::poll_wait(pt, 0);
            sync::poll_cleanup(pt);
        });
        g_repeat_progress.store_release(iter + 1);
    }
    sched::exit(0);
}

TEST(poll, repeated_poll_cycles) {
    g_repeat_wq.init();
    g_repeat_lock = sync::SPINLOCK_INIT;
    g_repeat_counter.store_relaxed(0);
    g_repeat_waiting.store_relaxed(0);
    g_repeat_progress.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(repeat_poll_fn, nullptr, "poll_rep");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    for (uint32_t iter = 0; iter < REPEAT_CYCLES; iter++) {
        ASSERT_TRUE(spin_wait_ge(g_repeat_waiting, iter + 1));
        brief_delay();
        RUN_ELEVATED({
            sync::wake_one(g_repeat_wq);
        });
        ASSERT_TRUE(spin_wait_ge(g_repeat_progress, iter + 1));
    }

    EXPECT_EQ(g_repeat_progress.load_acquire(), REPEAT_CYCLES);
}
