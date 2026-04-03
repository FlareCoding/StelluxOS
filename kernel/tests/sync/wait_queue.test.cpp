#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(wait_queue);

// --- basic_wait_wake_one ---
// One task blocks on a wait queue. The test sets a condition and
// calls wake_one. Verifies the task unblocks and completes.

static sync::wait_queue g_basic_wq;
static sync::spinlock g_basic_lock;
static volatile uint32_t g_basic_go;
static volatile uint32_t g_basic_waiting;
static volatile uint32_t g_basic_done;

static void basic_waiter_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_basic_lock);
        __atomic_store_n(&g_basic_waiting, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_basic_go, __ATOMIC_ACQUIRE)) {
            irq = sync::wait(g_basic_wq, g_basic_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_basic_lock, irq);
    });
    __atomic_store_n(&g_basic_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(wait_queue, basic_wait_wake_one) {
    g_basic_wq.init();
    g_basic_lock = sync::SPINLOCK_INIT;
    g_basic_go = 0;
    g_basic_waiting = 0;
    g_basic_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            basic_waiter_fn, nullptr, "wq_basic");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    ASSERT_TRUE(spin_wait(&g_basic_waiting));

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_basic_lock);
        __atomic_store_n(&g_basic_go, 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_basic_lock, irq);
        sync::wake_one(g_basic_wq);
    });

    EXPECT_TRUE(spin_wait(&g_basic_done));
}

// --- wake_one_empty_queue ---
// Calling wake_one on an empty queue is a safe no-op.

TEST(wait_queue, wake_one_empty_queue) {
    sync::wait_queue wq;
    wq.init();

    RUN_ELEVATED({
        sync::wake_one(wq);
    });

    EXPECT_TRUE(true);
}

// --- wake_all_empty_queue ---
// Calling wake_all on an empty queue is a safe no-op.

TEST(wait_queue, wake_all_empty_queue) {
    sync::wait_queue wq;
    wq.init();

    RUN_ELEVATED({
        sync::wake_all(wq);
    });

    EXPECT_TRUE(true);
}

// --- wake_all_wakes_everyone ---
// 4 tasks block on the same queue. A single wake_all unblocks all.

constexpr uint32_t MULTI_COUNT = 4;

static sync::wait_queue g_all_wq;
static sync::spinlock g_all_lock;
static volatile uint32_t g_all_go;
static volatile uint32_t g_all_ready[MULTI_COUNT];
static volatile uint32_t g_all_done[MULTI_COUNT];

static void wake_all_waiter_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_all_lock);
        __atomic_store_n(&g_all_ready[idx], 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_all_go, __ATOMIC_ACQUIRE)) {
            irq = sync::wait(g_all_wq, g_all_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_all_lock, irq);
    });

    __atomic_store_n(&g_all_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(wait_queue, wake_all_wakes_everyone) {
    g_all_wq.init();
    g_all_lock = sync::SPINLOCK_INIT;
    g_all_go = 0;
    for (uint32_t i = 0; i < MULTI_COUNT; i++) {
        g_all_ready[i] = 0;
        g_all_done[i] = 0;
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < MULTI_COUNT; i++) {
            sched::task* t = sched::create_kernel_task(
                wake_all_waiter_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "wq_all");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    for (uint32_t i = 0; i < MULTI_COUNT; i++) {
        ASSERT_TRUE(spin_wait(&g_all_ready[i]));
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_all_lock);
        __atomic_store_n(&g_all_go, 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_all_lock, irq);
        sync::wake_all(g_all_wq);
    });

    for (uint32_t i = 0; i < MULTI_COUNT; i++) {
        EXPECT_TRUE(spin_wait(&g_all_done[i]));
    }
}

// --- producer_consumer ---
// A producer increments a shared counter N times, calling wake_one
// after each increment. A consumer waits until the counter reaches N.
// Validates the canonical mutex/condvar usage pattern.

constexpr uint32_t PC_TARGET = 50;

static sync::wait_queue g_pc_wq;
static sync::spinlock g_pc_lock;
static volatile uint32_t g_pc_counter;
static volatile uint32_t g_pc_producer_done;
static volatile uint32_t g_pc_consumer_done;

static void pc_producer_fn(void*) {
    for (uint32_t i = 0; i < PC_TARGET; i++) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(g_pc_lock);
            __atomic_fetch_add(&g_pc_counter, 1, __ATOMIC_RELAXED);
            sync::spin_unlock_irqrestore(g_pc_lock, irq);
            sync::wake_one(g_pc_wq);
        });
    }
    __atomic_store_n(&g_pc_producer_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

static void pc_consumer_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_pc_lock);
        while (__atomic_load_n(&g_pc_counter, __ATOMIC_RELAXED) < PC_TARGET) {
            irq = sync::wait(g_pc_wq, g_pc_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_pc_lock, irq);
    });
    __atomic_store_n(&g_pc_consumer_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(wait_queue, producer_consumer) {
    g_pc_wq.init();
    g_pc_lock = sync::SPINLOCK_INIT;
    g_pc_counter = 0;
    g_pc_producer_done = 0;
    g_pc_consumer_done = 0;

    RUN_ELEVATED({
        sched::task* consumer = sched::create_kernel_task(
            pc_consumer_fn, nullptr, "wq_cons");
        sched::task* producer = sched::create_kernel_task(
            pc_producer_fn, nullptr, "wq_prod");
        ASSERT_NOT_NULL(consumer);
        ASSERT_NOT_NULL(producer);
        sched::enqueue(consumer);
        sched::enqueue(producer);
    });

    EXPECT_TRUE(spin_wait(&g_pc_producer_done));
    EXPECT_TRUE(spin_wait(&g_pc_consumer_done));
    EXPECT_EQ(__atomic_load_n(&g_pc_counter, __ATOMIC_ACQUIRE), PC_TARGET);
}

// --- repeated_wait_wake ---
// A single task blocks and is woken 10 times in sequence.
// Verifies that repeated wait/wake cycles don't corrupt state.

constexpr uint32_t REPEAT_COUNT = 10;

static sync::wait_queue g_repeat_wq;
static sync::spinlock g_repeat_lock;
static volatile uint32_t g_repeat_counter;
static volatile uint32_t g_repeat_progress;
static volatile uint32_t g_repeat_iter_waiting;

static void repeat_waiter_fn(void*) {
    for (uint32_t iter = 0; iter < REPEAT_COUNT; iter++) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(g_repeat_lock);
            __atomic_store_n(&g_repeat_iter_waiting, iter + 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&g_repeat_counter, __ATOMIC_ACQUIRE) <= iter) {
                irq = sync::wait(g_repeat_wq, g_repeat_lock, irq);
            }
            sync::spin_unlock_irqrestore(g_repeat_lock, irq);
        });
        __atomic_store_n(&g_repeat_progress, iter + 1, __ATOMIC_RELEASE);
    }
    sched::exit(0);
}

TEST(wait_queue, repeated_wait_wake) {
    g_repeat_wq.init();
    g_repeat_lock = sync::SPINLOCK_INIT;
    g_repeat_counter = 0;
    g_repeat_progress = 0;
    g_repeat_iter_waiting = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            repeat_waiter_fn, nullptr, "wq_repeat");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    for (uint32_t iter = 0; iter < REPEAT_COUNT; iter++) {
        ASSERT_TRUE(spin_wait_ge(&g_repeat_iter_waiting, iter + 1));

        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(g_repeat_lock);
            __atomic_fetch_add(&g_repeat_counter, 1, __ATOMIC_RELAXED);
            sync::spin_unlock_irqrestore(g_repeat_lock, irq);
            sync::wake_one(g_repeat_wq);
        });

        ASSERT_TRUE(spin_wait_ge(&g_repeat_progress, iter + 1));
    }

    EXPECT_EQ(__atomic_load_n(&g_repeat_progress, __ATOMIC_ACQUIRE), REPEAT_COUNT);
}

// --- wake_one_only_wakes_one ---
// 3 tasks block. wake_one is called three times, verifying that
// exactly one task wakes per call.

constexpr uint32_t WAKE_ONE_TASKS = 3;

static sync::wait_queue g_wone_wq;
static sync::spinlock g_wone_lock;
static volatile uint32_t g_wone_go;
static volatile uint32_t g_wone_ready_count;
static volatile uint32_t g_wone_done_count;

static void wake_one_worker_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_wone_lock);
        __atomic_fetch_add(&g_wone_ready_count, 1, __ATOMIC_ACQ_REL);
        while (!__atomic_load_n(&g_wone_go, __ATOMIC_ACQUIRE)) {
            irq = sync::wait(g_wone_wq, g_wone_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_wone_lock, irq);
    });
    __atomic_fetch_add(&g_wone_done_count, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(wait_queue, wake_one_only_wakes_one) {
    g_wone_wq.init();
    g_wone_lock = sync::SPINLOCK_INIT;
    g_wone_go = 0;
    g_wone_ready_count = 0;
    g_wone_done_count = 0;

    RUN_ELEVATED({
        for (uint32_t i = 0; i < WAKE_ONE_TASKS; i++) {
            sched::task* t = sched::create_kernel_task(
                wake_one_worker_fn, nullptr, "wq_one");
            ASSERT_NOT_NULL(t);
            sched::enqueue(t);
        }
    });

    ASSERT_TRUE(spin_wait_ge(&g_wone_ready_count, WAKE_ONE_TASKS));

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_wone_lock);
        __atomic_store_n(&g_wone_go, 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_wone_lock, irq);
    });

    for (uint32_t i = 0; i < WAKE_ONE_TASKS; i++) {
        RUN_ELEVATED({
            sync::wake_one(g_wone_wq);
        });
        ASSERT_TRUE(spin_wait_ge(&g_wone_done_count, i + 1));
        brief_delay();
        EXPECT_EQ(__atomic_load_n(&g_wone_done_count, __ATOMIC_ACQUIRE), i + 1);
    }
}

// --- wake_with_condition_recheck ---
// Two tasks wait with different conditions. wake_all is called, but
// only one condition is true. The task whose condition is false must
// re-block (the while-loop recheck pattern). Then the second condition
// is set and wake_all is called again.

static sync::wait_queue g_recheck_wq;
static sync::spinlock g_recheck_lock;
static volatile uint32_t g_recheck_go[2];
static volatile uint32_t g_recheck_ready[2];
static volatile uint32_t g_recheck_done_count;

static void recheck_waiter_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_recheck_lock);
        __atomic_store_n(&g_recheck_ready[idx], 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_recheck_go[idx], __ATOMIC_ACQUIRE)) {
            irq = sync::wait(g_recheck_wq, g_recheck_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_recheck_lock, irq);
    });

    __atomic_fetch_add(&g_recheck_done_count, 1, __ATOMIC_ACQ_REL);
    sched::exit(0);
}

TEST(wait_queue, wake_with_condition_recheck) {
    g_recheck_wq.init();
    g_recheck_lock = sync::SPINLOCK_INIT;
    g_recheck_go[0] = 0;
    g_recheck_go[1] = 0;
    g_recheck_ready[0] = 0;
    g_recheck_ready[1] = 0;
    g_recheck_done_count = 0;

    RUN_ELEVATED({
        sched::task* t0 = sched::create_kernel_task(
            recheck_waiter_fn,
            reinterpret_cast<void*>(static_cast<uintptr_t>(0)),
            "wq_recheck");
        sched::task* t1 = sched::create_kernel_task(
            recheck_waiter_fn,
            reinterpret_cast<void*>(static_cast<uintptr_t>(1)),
            "wq_recheck");
        ASSERT_NOT_NULL(t0);
        ASSERT_NOT_NULL(t1);
        sched::enqueue(t0);
        sched::enqueue(t1);
    });

    ASSERT_TRUE(spin_wait(&g_recheck_ready[0]));
    ASSERT_TRUE(spin_wait(&g_recheck_ready[1]));

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_recheck_lock);
        __atomic_store_n(&g_recheck_go[0], 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_recheck_lock, irq);
        sync::wake_all(g_recheck_wq);
    });

    ASSERT_TRUE(spin_wait_ge(&g_recheck_done_count, 1));
    brief_delay();
    EXPECT_EQ(__atomic_load_n(&g_recheck_done_count, __ATOMIC_ACQUIRE), 1u);

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_recheck_lock);
        __atomic_store_n(&g_recheck_go[1], 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_recheck_lock, irq);
        sync::wake_all(g_recheck_wq);
    });

    ASSERT_TRUE(spin_wait_ge(&g_recheck_done_count, 2));
    EXPECT_EQ(__atomic_load_n(&g_recheck_done_count, __ATOMIC_ACQUIRE), 2u);
}
