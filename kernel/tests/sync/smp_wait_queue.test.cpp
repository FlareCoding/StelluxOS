#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "dynpriv/dynpriv.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::brief_delay;

TEST_SUITE(smp_wait_queue);

constexpr uint32_t MAX_TEST_CPUS = 16;

// --- cross_cpu_wake_one ---
// Proves: a task blocked on CPU 1 is woken by wake_one() from CPU 0.
// Uses the condition-variable pattern (while-loop re-check) to handle
// the race where wake_one fires before the task enters the wait queue.

static sync::wait_queue g_xwake_wq;
static sync::spinlock g_xwake_lock;
static volatile uint32_t g_xwake_go;
static volatile uint32_t g_xwake_waiting;
static volatile uint32_t g_xwake_done;

static void xwake_waiter_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xwake_lock);
        __atomic_store_n(&g_xwake_waiting, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_xwake_go, __ATOMIC_ACQUIRE)) {
            irq = sync::wait(g_xwake_wq, g_xwake_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_xwake_lock, irq);
    });
    __atomic_store_n(&g_xwake_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_wait_queue, cross_cpu_wake_one) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xwake_wq.init();
    g_xwake_lock = sync::SPINLOCK_INIT;
    g_xwake_go = 0;
    g_xwake_waiting = 0;
    g_xwake_done = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            xwake_waiter_fn, nullptr, "smp_wq1");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(&g_xwake_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xwake_lock);
        __atomic_store_n(&g_xwake_go, 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_xwake_lock, irq);
        sync::wake_one(g_xwake_wq);
    });

    EXPECT_TRUE(spin_wait(&g_xwake_done));
}

// --- cross_cpu_wake_all ---
// Proves: wake_all unblocks tasks spread across multiple CPUs.
// One task per non-BSP CPU blocks on the same wait queue.

static sync::wait_queue g_xall_wq;
static sync::spinlock g_xall_lock;
static volatile uint32_t g_xall_go;
static volatile uint32_t g_xall_ready[MAX_TEST_CPUS] = {};
static volatile uint32_t g_xall_done[MAX_TEST_CPUS] = {};

static void xall_waiter_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xall_lock);
        __atomic_store_n(&g_xall_ready[idx], 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_xall_go, __ATOMIC_ACQUIRE)) {
            irq = sync::wait(g_xall_wq, g_xall_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_xall_lock, irq);
    });
    __atomic_store_n(&g_xall_done[idx], 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_wait_queue, cross_cpu_wake_all) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    g_xall_wq.init();
    g_xall_lock = sync::SPINLOCK_INIT;
    g_xall_go = 0;

    uint32_t waiter_count = cpus - 1;
    for (uint32_t i = 0; i < waiter_count; i++) {
        g_xall_ready[i] = 0;
        g_xall_done[i] = 0;
    }

    RUN_ELEVATED({
        for (uint32_t i = 0; i < waiter_count; i++) {
            sched::task* t = sched::create_kernel_task(
                xall_waiter_fn,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
                "smp_wqa");
            ASSERT_NOT_NULL(t);
            sched::enqueue_on(t, i + 1);
        }
    });

    for (uint32_t i = 0; i < waiter_count; i++) {
        ASSERT_TRUE(spin_wait(&g_xall_ready[i]));
    }
    brief_delay();

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xall_lock);
        __atomic_store_n(&g_xall_go, 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_xall_lock, irq);
        sync::wake_all(g_xall_wq);
    });

    for (uint32_t i = 0; i < waiter_count; i++) {
        EXPECT_TRUE(spin_wait(&g_xall_done[i]));
    }
}

// --- cross_cpu_producer_consumer ---
// Proves: producer on CPU 0 and consumer on CPU 1 communicate via wait queue.
// Consumer signals "ready" before entering the wait loop so the producer
// doesn't start before the consumer is listening.

constexpr uint32_t PC_TARGET = 50;

static sync::wait_queue g_xpc_wq;
static sync::spinlock g_xpc_lock;
static volatile uint32_t g_xpc_counter;
static volatile uint32_t g_xpc_consumer_ready;
static volatile uint32_t g_xpc_producer_done;
static volatile uint32_t g_xpc_consumer_done;

static void xpc_consumer_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xpc_lock);
        __atomic_store_n(&g_xpc_consumer_ready, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_xpc_counter, __ATOMIC_RELAXED) < PC_TARGET) {
            irq = sync::wait(g_xpc_wq, g_xpc_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_xpc_lock, irq);
    });
    __atomic_store_n(&g_xpc_consumer_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

static void xpc_producer_fn(void*) {
    while (!__atomic_load_n(&g_xpc_consumer_ready, __ATOMIC_ACQUIRE)) {}

    for (uint32_t i = 0; i < PC_TARGET; i++) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(g_xpc_lock);
            __atomic_fetch_add(&g_xpc_counter, 1, __ATOMIC_RELAXED);
            sync::spin_unlock_irqrestore(g_xpc_lock, irq);
            sync::wake_one(g_xpc_wq);
        });
    }
    __atomic_store_n(&g_xpc_producer_done, 1, __ATOMIC_RELEASE);
    sched::exit(0);
}

TEST(smp_wait_queue, cross_cpu_producer_consumer) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xpc_wq.init();
    g_xpc_lock = sync::SPINLOCK_INIT;
    g_xpc_counter = 0;
    g_xpc_consumer_ready = 0;
    g_xpc_producer_done = 0;
    g_xpc_consumer_done = 0;

    RUN_ELEVATED({
        sched::task* consumer = sched::create_kernel_task(
            xpc_consumer_fn, nullptr, "smp_cons");
        sched::task* producer = sched::create_kernel_task(
            xpc_producer_fn, nullptr, "smp_prod");
        ASSERT_NOT_NULL(consumer);
        ASSERT_NOT_NULL(producer);
        sched::enqueue_on(consumer, 1);
        sched::enqueue_on(producer, 0);
    });

    EXPECT_TRUE(spin_wait(&g_xpc_producer_done));
    EXPECT_TRUE(spin_wait(&g_xpc_consumer_done));
    EXPECT_EQ(__atomic_load_n(&g_xpc_counter, __ATOMIC_ACQUIRE), PC_TARGET);
}
