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

// ---------------------------------------------------------------------------
// basic_subscribe_and_trigger
// One task subscribes to a single wait queue, another fires it.
// ---------------------------------------------------------------------------

static sync::wait_queue g_basic_wq;
static volatile uint32_t g_basic_waiting;
static volatile uint32_t g_basic_result;
static volatile uint32_t g_basic_done;

static void basic_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entry = {};
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_basic_wq, entry);

        __atomic_store_n(&g_basic_waiting, 1, __ATOMIC_RELEASE);
        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_basic_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_basic_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll, basic_subscribe_and_trigger) {
    g_basic_wq.init();
    g_basic_waiting = 0;
    g_basic_result = 0;
    g_basic_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(basic_poll_fn, nullptr, "poll_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_basic_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_basic_wq);
    });

    ASSERT_TRUE(spin_wait(&g_basic_done));
    EXPECT_EQ(__atomic_load_n(&g_basic_result, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// multi_source_first_fires
// Subscribe to 3 wait queues, fire the first one.
// ---------------------------------------------------------------------------

static sync::wait_queue g_multi_wq[3];
static volatile uint32_t g_multi_first_waiting;
static volatile uint32_t g_multi_first_result;
static volatile uint32_t g_multi_first_done;

static void multi_first_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entries[3] = {};
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_multi_wq[i], entries[i]);
        }

        __atomic_store_n(&g_multi_first_waiting, 1, __ATOMIC_RELEASE);
        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_multi_first_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_multi_first_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll, multi_source_first_fires) {
    for (int i = 0; i < 3; i++) g_multi_wq[i].init();
    g_multi_first_waiting = 0;
    g_multi_first_result = 0;
    g_multi_first_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(multi_first_fn, nullptr, "poll_mf");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_multi_first_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_multi_wq[0]);
    });

    ASSERT_TRUE(spin_wait(&g_multi_first_done));
    EXPECT_EQ(__atomic_load_n(&g_multi_first_result, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// multi_source_last_fires
// Subscribe to 3 wait queues, fire the last one.
// ---------------------------------------------------------------------------

static volatile uint32_t g_multi_last_waiting;
static volatile uint32_t g_multi_last_result;
static volatile uint32_t g_multi_last_done;

static void multi_last_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entries[3] = {};
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_multi_wq[i], entries[i]);
        }

        __atomic_store_n(&g_multi_last_waiting, 1, __ATOMIC_RELEASE);
        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_multi_last_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_multi_last_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll, multi_source_last_fires) {
    for (int i = 0; i < 3; i++) g_multi_wq[i].init();
    g_multi_last_waiting = 0;
    g_multi_last_result = 0;
    g_multi_last_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(multi_last_fn, nullptr, "poll_ml");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_multi_last_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_multi_wq[2]);
    });

    ASSERT_TRUE(spin_wait(&g_multi_last_done));
    EXPECT_EQ(__atomic_load_n(&g_multi_last_result, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// multi_source_all_fire
// Subscribe to 3 wait queues, fire all of them. Task should wake once.
// ---------------------------------------------------------------------------

static volatile uint32_t g_multi_all_waiting;
static volatile uint32_t g_multi_all_result;
static volatile uint32_t g_multi_all_done;

static void multi_all_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entries[3] = {};
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, g_multi_wq[i], entries[i]);
        }

        __atomic_store_n(&g_multi_all_waiting, 1, __ATOMIC_RELEASE);
        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_multi_all_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_multi_all_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll, multi_source_all_fire) {
    for (int i = 0; i < 3; i++) g_multi_wq[i].init();
    g_multi_all_waiting = 0;
    g_multi_all_result = 0;
    g_multi_all_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(multi_all_fn, nullptr, "poll_ma");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_multi_all_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::wake_one(g_multi_wq[0]);
        sync::wake_one(g_multi_wq[1]);
        sync::wake_one(g_multi_wq[2]);
    });

    ASSERT_TRUE(spin_wait(&g_multi_all_done));
    EXPECT_EQ(__atomic_load_n(&g_multi_all_result, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// timeout_no_trigger
// Subscribe but never fire. Wait with timeout. Verify returns false.
// ---------------------------------------------------------------------------

static volatile uint32_t g_timeout_result;
static volatile uint32_t g_timeout_done;

static void timeout_fn(void*) {
    RUN_ELEVATED({
        sync::wait_queue wq;
        wq.init();
        sync::poll_table pt;
        sync::poll_entry entry = {};
        pt.init(sched::current());
        sync::poll_subscribe(pt, wq, entry);

        bool triggered = sync::poll_wait(pt, 50000000ULL); // 50ms
        __atomic_store_n(&g_timeout_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_timeout_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll, timeout_no_trigger) {
    g_timeout_result = 0;
    g_timeout_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(timeout_fn, nullptr, "poll_to");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_timeout_done));
    EXPECT_EQ(__atomic_load_n(&g_timeout_result, __ATOMIC_ACQUIRE), 0u);
}

// ---------------------------------------------------------------------------
// immediate_trigger_no_block
// Fire the source before poll_wait — should return immediately.
// ---------------------------------------------------------------------------

static volatile uint32_t g_imm_result;
static volatile uint32_t g_imm_done;

static void immediate_fn(void*) {
    RUN_ELEVATED({
        sync::wait_queue wq;
        wq.init();
        sync::poll_table pt;
        sync::poll_entry entry = {};
        pt.init(sched::current());
        sync::poll_subscribe(pt, wq, entry);

        // Fire the source before waiting
        sync::wake_one(wq);

        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_imm_result, triggered ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_imm_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll, immediate_trigger_no_block) {
    g_imm_result = 0;
    g_imm_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(immediate_fn, nullptr, "poll_imm");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_imm_done));
    EXPECT_EQ(__atomic_load_n(&g_imm_result, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// cleanup_removes_all_observers
// Subscribe to 3 wqs, cleanup, verify observer lists are empty.
// ---------------------------------------------------------------------------

static volatile uint32_t g_cleanup_done;

static void cleanup_fn(void*) {
    RUN_ELEVATED({
        sync::wait_queue wqs[3];
        for (int i = 0; i < 3; i++) wqs[i].init();

        sync::poll_table pt;
        sync::poll_entry entries[3] = {};
        pt.init(sched::current());
        for (int i = 0; i < 3; i++) {
            sync::poll_subscribe(pt, wqs[i], entries[i]);
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
            __atomic_store_n(&g_cleanup_done, 2, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&g_cleanup_done, 1, __ATOMIC_RELEASE);
        }
    });
    sched::exit(0);
}

TEST(poll, cleanup_removes_all_observers) {
    g_cleanup_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(cleanup_fn, nullptr, "poll_cl");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_cleanup_done));
    EXPECT_EQ(__atomic_load_n(&g_cleanup_done, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// kill_pending_wakes_poll
// Task in poll_wait, force_wake_for_kill wakes it.
// ---------------------------------------------------------------------------

static sync::wait_queue g_kill_wq;
static volatile uint32_t g_kill_waiting;
static volatile uint32_t g_kill_result;
static volatile uint32_t g_kill_kp;
static volatile uint32_t g_kill_done;

static void kill_poll_fn(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        sync::poll_entry entry = {};
        pt.init(sched::current());
        sync::poll_subscribe(pt, g_kill_wq, entry);

        __atomic_store_n(&g_kill_waiting, 1, __ATOMIC_RELEASE);
        bool triggered = sync::poll_wait(pt, 0);
        __atomic_store_n(&g_kill_result, triggered ? 1 : 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_kill_kp,
            sched::is_kill_pending() ? 1 : 0, __ATOMIC_RELEASE);

        sync::poll_cleanup(pt);
    });
    __atomic_store_n(&g_kill_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(poll, kill_pending_wakes_poll) {
    g_kill_wq.init();
    g_kill_waiting = 0;
    g_kill_result = 0;
    g_kill_kp = 0;
    g_kill_done = 0;

    sched::task* t = nullptr;
    RUN_ELEVATED({
        t = sched::create_kernel_task(kill_poll_fn, nullptr, "poll_kill");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_kill_waiting));
    brief_delay();

    RUN_ELEVATED({
        sched::force_wake_for_kill(t);
    });

    ASSERT_TRUE(spin_wait(&g_kill_done));
    // Source was not fired, so triggered should be 0
    EXPECT_EQ(__atomic_load_n(&g_kill_result, __ATOMIC_ACQUIRE), 0u);
    EXPECT_EQ(__atomic_load_n(&g_kill_kp, __ATOMIC_ACQUIRE), 1u);
}

// ---------------------------------------------------------------------------
// repeated_poll_cycles
// Subscribe/wait/cleanup 10 times on the same wqs.
// ---------------------------------------------------------------------------

static sync::wait_queue g_repeat_wq;
static sync::spinlock g_repeat_lock;
static volatile uint32_t g_repeat_counter;
static volatile uint32_t g_repeat_waiting;
static volatile uint32_t g_repeat_progress;

constexpr uint32_t REPEAT_CYCLES = 10;

static void repeat_poll_fn(void*) {
    for (uint32_t iter = 0; iter < REPEAT_CYCLES; iter++) {
        RUN_ELEVATED({
            sync::poll_table pt;
            sync::poll_entry entry = {};
            pt.init(sched::current());
            sync::poll_subscribe(pt, g_repeat_wq, entry);

            __atomic_store_n(&g_repeat_waiting, iter + 1, __ATOMIC_RELEASE);
            sync::poll_wait(pt, 0);
            sync::poll_cleanup(pt);
        });
        __atomic_store_n(&g_repeat_progress, iter + 1, __ATOMIC_RELEASE);
    }
    sched::exit(0);
}

TEST(poll, repeated_poll_cycles) {
    g_repeat_wq.init();
    g_repeat_lock = sync::SPINLOCK_INIT;
    g_repeat_counter = 0;
    g_repeat_waiting = 0;
    g_repeat_progress = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(repeat_poll_fn, nullptr, "poll_rep");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    for (uint32_t iter = 0; iter < REPEAT_CYCLES; iter++) {
        ASSERT_TRUE(spin_wait_ge(&g_repeat_waiting, iter + 1));
        brief_delay();
        RUN_ELEVATED({
            sync::wake_one(g_repeat_wq);
        });
        ASSERT_TRUE(spin_wait_ge(&g_repeat_progress, iter + 1));
    }

    EXPECT_EQ(__atomic_load_n(&g_repeat_progress, __ATOMIC_ACQUIRE), REPEAT_CYCLES);
}
