#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

using test_helpers::spin_wait;
using test_helpers::brief_delay;

TEST_SUITE(termination);

// --- request_running_task ---
// Proves: a running task can observe a pending termination request at a
// cooperative safe point and exit through terminate_if_requested().

static volatile uint32_t g_run_started = 0;
static volatile uint32_t g_run_seen_request = 0;
static volatile uint32_t g_run_request_returned = 0;
static volatile uint32_t g_run_iterations = 0;
static volatile int32_t  g_run_exit_code_seen = 0;

static void running_task_fn(void*) {
    __atomic_store_n(&g_run_started, 1, __ATOMIC_RELEASE);

    for (;;) {
        __atomic_fetch_add(&g_run_iterations, 1, __ATOMIC_RELAXED);
        RUN_ELEVATED({
            if (sched::termination_requested()) {
                __atomic_store_n(&g_run_exit_code_seen,
                                 sched::current()->termination_exit_code,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&g_run_seen_request, 1, __ATOMIC_RELEASE);
                sched::terminate_if_requested();
            }
        });
        sched::yield();
    }
}

TEST(termination, request_running_task) {
    g_run_started = 0;
    g_run_seen_request = 0;
    g_run_request_returned = 0;
    g_run_iterations = 0;
    g_run_exit_code_seen = 0;

    sched::task* worker = nullptr;
    RUN_ELEVATED({
        worker = sched::create_kernel_task(
            running_task_fn, nullptr, "term_run", sched::TASK_FLAG_ELEVATED);
        ASSERT_NOT_NULL(worker);
        sched::enqueue(worker);
    });

    ASSERT_TRUE(spin_wait(&g_run_started));

    RUN_ELEVATED({
        bool first = sched::request_task_terminate(worker, 137);
        bool second = sched::request_task_terminate(worker, 222);
        EXPECT_TRUE(first);
        EXPECT_FALSE(second);
        __atomic_store_n(&g_run_request_returned, 1, __ATOMIC_RELEASE);
    });
    sched::yield();

    ASSERT_TRUE(spin_wait(&g_run_seen_request));
    EXPECT_TRUE(__atomic_load_n(&g_run_request_returned, __ATOMIC_ACQUIRE) != 0);
    EXPECT_GE(__atomic_load_n(&g_run_iterations, __ATOMIC_ACQUIRE), 1u);
    EXPECT_EQ(__atomic_load_n(&g_run_exit_code_seen, __ATOMIC_ACQUIRE), 137);
}

// --- request_wait_queue_blocked_task ---
// Proves: request_task_terminate removes a blocked waiter, the waiter sees the
// request after wake, and the intrusive wait link is cleared.

static sync::wait_queue g_wait_wq;
static sync::spinlock g_wait_lock;
static volatile uint32_t g_wait_ready = 0;
static volatile uint32_t g_wait_seen_request = 0;
static volatile uint32_t g_wait_request_returned = 0;
static volatile uint32_t g_wait_link_cleared = 0;
static volatile int32_t  g_wait_exit_code_seen = 0;

static void wait_blocked_task_fn(void*) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_wait_lock);
        __atomic_store_n(&g_wait_ready, 1, __ATOMIC_RELEASE);

        while (!sched::termination_requested()) {
            irq = sync::wait(g_wait_wq, g_wait_lock, irq);
        }

        sched::task* self = sched::current();
        bool link_cleared =
            self->wait_link.prev == nullptr && self->wait_link.next == nullptr;
        __atomic_store_n(&g_wait_link_cleared, link_cleared ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_wait_exit_code_seen,
                         self->termination_exit_code,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_wait_seen_request, 1, __ATOMIC_RELEASE);
        sync::spin_unlock_irqrestore(g_wait_lock, irq);
    });

    RUN_ELEVATED({
        sched::terminate_if_requested();
    });
    sched::exit(99);
}

TEST(termination, request_wait_queue_blocked_task) {
    g_wait_wq.init();
    g_wait_lock = sync::SPINLOCK_INIT;
    g_wait_ready = 0;
    g_wait_seen_request = 0;
    g_wait_request_returned = 0;
    g_wait_link_cleared = 0;
    g_wait_exit_code_seen = 0;

    sched::task* worker = nullptr;
    RUN_ELEVATED({
        worker = sched::create_kernel_task(wait_blocked_task_fn, nullptr, "term_wait");
        ASSERT_NOT_NULL(worker);
        sched::enqueue(worker);
    });

    ASSERT_TRUE(spin_wait(&g_wait_ready));

    RUN_ELEVATED({
        bool first = sched::request_task_terminate(worker, 137);
        bool second = sched::request_task_terminate(worker, 444);
        EXPECT_TRUE(first);
        EXPECT_FALSE(second);
        __atomic_store_n(&g_wait_request_returned, 1, __ATOMIC_RELEASE);
    });

    ASSERT_TRUE(spin_wait(&g_wait_seen_request));
    EXPECT_TRUE(__atomic_load_n(&g_wait_request_returned, __ATOMIC_ACQUIRE) != 0);
    EXPECT_TRUE(__atomic_load_n(&g_wait_link_cleared, __ATOMIC_ACQUIRE) != 0);
    EXPECT_EQ(__atomic_load_n(&g_wait_exit_code_seen, __ATOMIC_ACQUIRE), 137);
}

// --- request_timer_sleep_task ---
// Proves: request_task_terminate cancels a sleeping task, clears timer linkage,
// and lets the task observe the request after sleep returns.

static volatile uint32_t g_sleep_ready = 0;
static volatile uint32_t g_sleep_seen_request = 0;
static volatile uint32_t g_sleep_request_returned = 0;
static volatile uint32_t g_sleep_link_cleared = 0;
static volatile int32_t  g_sleep_exit_code_seen = 0;

static void sleeping_task_fn(void*) {
    __atomic_store_n(&g_sleep_ready, 1, __ATOMIC_RELEASE);

    for (;;) {
        RUN_ELEVATED({
            sched::sleep_ns(1000000000ULL);
            if (sched::termination_requested()) {
                sched::task* self = sched::current();
                bool link_cleared =
                    self->timer_link.prev == nullptr &&
                    self->timer_link.next == nullptr &&
                    self->timer_deadline == 0;
                __atomic_store_n(&g_sleep_link_cleared, link_cleared ? 1u : 0u, __ATOMIC_RELEASE);
                __atomic_store_n(&g_sleep_exit_code_seen,
                                 self->termination_exit_code,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&g_sleep_seen_request, 1, __ATOMIC_RELEASE);
                sched::terminate_if_requested();
            }
        });
    }
}

TEST(termination, request_timer_sleep_task) {
    g_sleep_ready = 0;
    g_sleep_seen_request = 0;
    g_sleep_request_returned = 0;
    g_sleep_link_cleared = 0;
    g_sleep_exit_code_seen = 0;

    sched::task* worker = nullptr;
    RUN_ELEVATED({
        worker = sched::create_kernel_task(sleeping_task_fn, nullptr, "term_sleep");
        ASSERT_NOT_NULL(worker);
        sched::enqueue(worker);
    });

    ASSERT_TRUE(spin_wait(&g_sleep_ready));
    brief_delay();

    RUN_ELEVATED({
        bool first = sched::request_task_terminate(worker, 137);
        bool second = sched::request_task_terminate(worker, 555);
        EXPECT_TRUE(first);
        EXPECT_FALSE(second);
        __atomic_store_n(&g_sleep_request_returned, 1, __ATOMIC_RELEASE);
    });

    ASSERT_TRUE(spin_wait(&g_sleep_seen_request));
    EXPECT_TRUE(__atomic_load_n(&g_sleep_request_returned, __ATOMIC_ACQUIRE) != 0);
    EXPECT_TRUE(__atomic_load_n(&g_sleep_link_cleared, __ATOMIC_ACQUIRE) != 0);
    EXPECT_EQ(__atomic_load_n(&g_sleep_exit_code_seen, __ATOMIC_ACQUIRE), 137);
}
