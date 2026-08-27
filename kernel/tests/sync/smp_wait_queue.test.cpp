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
// The while-loop recheck covers a wake that fires before the wait.

static sync::wait_queue g_xwake_wq;
static sync::spinlock g_xwake_lock;
static sync::atomic<uint32_t> g_xwake_go;
static sync::atomic<uint32_t> g_xwake_waiting;
static sync::atomic<uint32_t> g_xwake_done;

static void xwake_waiter_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xwake_lock);
        g_xwake_waiting.store_release(1);
        while (!g_xwake_go.load_acquire()) {
            irq = sync::wait(g_xwake_wq, g_xwake_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_xwake_lock, irq);
    });
    g_xwake_done.store_release(1);
    sched::exit(0);
}

TEST(smp_wait_queue, cross_cpu_wake_one) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xwake_wq.init();
    g_xwake_lock = sync::SPINLOCK_INIT;
    g_xwake_go.store_relaxed(0);
    g_xwake_waiting.store_relaxed(0);
    g_xwake_done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(
            xwake_waiter_fn, nullptr, "smp_wq1");
        ASSERT_NOT_NULL(t);
        sched::enqueue_on(t, 1);
    });

    ASSERT_TRUE(spin_wait(g_xwake_waiting));
    brief_delay();

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xwake_lock);
        g_xwake_go.store_release(1);
        sync::spin_unlock_irqrestore(g_xwake_lock, irq);
        sync::wake_one(g_xwake_wq);
    });

    EXPECT_TRUE(spin_wait(g_xwake_done));
}

// --- cross_cpu_wake_all ---
// Proves: wake_all unblocks tasks spread across multiple CPUs.
// One task per non-BSP CPU blocks on the same wait queue.

static sync::wait_queue g_xall_wq;
static sync::spinlock g_xall_lock;
static sync::atomic<uint32_t> g_xall_go;
static sync::atomic<uint32_t> g_xall_ready[MAX_TEST_CPUS] = {};
static sync::atomic<uint32_t> g_xall_done[MAX_TEST_CPUS] = {};

static void xall_waiter_fn(void* arg) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xall_lock);
        g_xall_ready[idx].store_release(1);
        while (!g_xall_go.load_acquire()) {
            irq = sync::wait(g_xall_wq, g_xall_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_xall_lock, irq);
    });
    g_xall_done[idx].store_release(1);
    sched::exit(0);
}

TEST(smp_wait_queue, cross_cpu_wake_all) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2 || cpus > MAX_TEST_CPUS) return;

    g_xall_wq.init();
    g_xall_lock = sync::SPINLOCK_INIT;
    g_xall_go.store_relaxed(0);

    uint32_t waiter_count = cpus - 1;
    for (uint32_t i = 0; i < waiter_count; i++) {
        g_xall_ready[i].store_relaxed(0);
        g_xall_done[i].store_relaxed(0);
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
        ASSERT_TRUE(spin_wait(g_xall_ready[i]));
    }
    brief_delay();

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xall_lock);
        g_xall_go.store_release(1);
        sync::spin_unlock_irqrestore(g_xall_lock, irq);
        sync::wake_all(g_xall_wq);
    });

    for (uint32_t i = 0; i < waiter_count; i++) {
        EXPECT_TRUE(spin_wait(g_xall_done[i]));
    }
}

// --- cross_cpu_producer_consumer ---
// Proves: producer on CPU 0 and consumer on CPU 1 communicate via wait
// queue, the consumer signals ready before the producer starts.

constexpr uint32_t PC_TARGET = 50;

static sync::wait_queue g_xpc_wq;
static sync::spinlock g_xpc_lock;
static sync::atomic<uint32_t> g_xpc_counter;
static sync::atomic<uint32_t> g_xpc_consumer_ready;
static sync::atomic<uint32_t> g_xpc_producer_done;
static sync::atomic<uint32_t> g_xpc_consumer_done;

static void xpc_consumer_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_xpc_lock);
        g_xpc_consumer_ready.store_release(1);
        while (g_xpc_counter.load_relaxed() < PC_TARGET) {
            irq = sync::wait(g_xpc_wq, g_xpc_lock, irq);
        }
        sync::spin_unlock_irqrestore(g_xpc_lock, irq);
    });
    g_xpc_consumer_done.store_release(1);
    sched::exit(0);
}

static void xpc_producer_fn(void*) {
    while (!g_xpc_consumer_ready.load_acquire()) {}

    for (uint32_t i = 0; i < PC_TARGET; i++) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(g_xpc_lock);
            g_xpc_counter.fetch_add_relaxed(1);
            sync::spin_unlock_irqrestore(g_xpc_lock, irq);
            sync::wake_one(g_xpc_wq);
        });
    }
    g_xpc_producer_done.store_release(1);
    sched::exit(0);
}

TEST(smp_wait_queue, cross_cpu_producer_consumer) {
    uint32_t cpus = smp::cpu_count();
    if (cpus < 2) return;

    g_xpc_wq.init();
    g_xpc_lock = sync::SPINLOCK_INIT;
    g_xpc_counter.store_relaxed(0);
    g_xpc_consumer_ready.store_relaxed(0);
    g_xpc_producer_done.store_relaxed(0);
    g_xpc_consumer_done.store_relaxed(0);

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

    EXPECT_TRUE(spin_wait(g_xpc_producer_done));
    EXPECT_TRUE(spin_wait(g_xpc_consumer_done));
    EXPECT_EQ(g_xpc_counter.load_acquire(), PC_TARGET);
}
