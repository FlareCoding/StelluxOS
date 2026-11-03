#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "clock/clock.h"
#include "drivers/pci_driver.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

TEST_SUITE(pci_driver_events);

using test_helpers::spin_wait;

constexpr uint64_t SHORT_DEADLINE_NS = 20000000ULL; // 20ms
constexpr uint64_t LONG_DEADLINE_NS  = 10000000000ULL; // 10s

// A driver with no device, whose interrupt handler counts its calls and returns `has_work`. A test raises its event the
// way an interrupt does
class event_test_driver : public drivers::pci_driver {
public:
    event_test_driver() : pci_driver("event_test", nullptr) {}

    int32_t attach() override { return drivers::OK; }
    void run() override {}

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE bool on_interrupt(uint32_t) override {
        handled++;
        return has_work;
    }

    using pci_driver::handle_interrupt;
    using pci_driver::wait_for_event_until;

    bool     has_work = false;
    uint32_t handled = 0;

    // Marks the event pending and wakes a waiter, as `pci_driver` does once a driver's interrupt is handled
    void raise_event() {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_irq_lock);
            m_event_pending = true;
            sync::wake_one(m_irq_wq);
            sync::spin_unlock_irqrestore(m_irq_lock, irq);
        });
    }

    // Whether the event is still pending, which a wait clears when it returns
    bool is_event_pending() const { return m_event_pending; }
};

static event_test_driver* g_driver;
static uint64_t g_deadline;
static uint64_t g_called_at;
static uint64_t g_returned_at;
static sync::atomic<uint32_t> g_returned;

// Records when the wait began and when it returned, then ends its task
static void waiter_fn(void*) {
    g_called_at = clock::now_ns();
    g_driver->wait_for_event_until(g_deadline);
    g_returned_at = clock::now_ns();

    g_returned.store_release(1);
    sched::exit(0);
}

// Runs the wait on a task of its own, since the test runner's task cannot sleep, and reports whether it returned before
// `spin_wait` gave up. A wait that never returns leaves its test's driver allocated, as the task still reads it
static bool wait_on_task(event_test_driver* driver, uint64_t deadline) {
    g_driver = driver;
    g_deadline = deadline;
    g_returned.store_relaxed(0);

    bool started = false;
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(waiter_fn, nullptr, "pci_event_wait");
        if (t) {
            sched::enqueue(t);
            started = true;
        }
    });

    return started && spin_wait(g_returned);
}

// A handler with no work leaves the event clear, and one with work marks it pending for the next wait to take
TEST(pci_driver_events, handle_interrupt_marks_the_event_only_with_work) {
    auto* driver = heap::ualloc_new<event_test_driver>();
    ASSERT_NOT_NULL(driver);

    RUN_ELEVATED(driver->handle_interrupt(0));
    EXPECT_EQ(driver->handled, 1u);
    EXPECT_FALSE(driver->is_event_pending());

    driver->has_work = true;
    RUN_ELEVATED(driver->handle_interrupt(0));
    EXPECT_EQ(driver->handled, 2u);
    EXPECT_TRUE(driver->is_event_pending());
    heap::ufree_delete(driver);
}

// A waiting task sleeps through a handler with no work and wakes, long before its deadline, on one with work
TEST(pci_driver_events, handle_interrupt_wakes_the_waiter_only_with_work) {
    auto* driver = heap::ualloc_new<event_test_driver>();
    ASSERT_NOT_NULL(driver);
    g_driver = driver;
    g_deadline = clock::now_ns() + LONG_DEADLINE_NS;
    g_returned.store_relaxed(0);

    sched::task* waiter = test_helpers::start_pinned_task(waiter_fn, nullptr, "pci_event_wait");
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));

    RUN_ELEVATED(driver->handle_interrupt(0));
    test_helpers::brief_delay();
    EXPECT_EQ(g_returned.load_acquire(), 0u);
    EXPECT_EQ(waiter->state.load_acquire(), sched::TASK_STATE_BLOCKED);

    driver->has_work = true;
    RUN_ELEVATED(driver->handle_interrupt(0));
    ASSERT_TRUE(spin_wait(g_returned));

    EXPECT_LT(g_returned_at, g_deadline);
    EXPECT_FALSE(driver->is_event_pending());
    test_helpers::unpin(waiter);
    heap::ufree_delete(driver);
}

// Nothing raises the event, so only the deadline ends the wait
TEST(pci_driver_events, wait_until_returns_at_deadline) {
    auto* driver = heap::ualloc_new<event_test_driver>();
    ASSERT_NOT_NULL(driver);

    uint64_t deadline = clock::now_ns() + SHORT_DEADLINE_NS;
    ASSERT_TRUE(wait_on_task(driver, deadline));

    EXPECT_GE(g_returned_at, deadline);
    heap::ufree_delete(driver);
}

// An event raised before the wait ends it at once, long before the deadline, and the wait takes the event
TEST(pci_driver_events, wait_until_takes_a_pending_event_at_once) {
    auto* driver = heap::ualloc_new<event_test_driver>();
    ASSERT_NOT_NULL(driver);

    driver->raise_event();
    ASSERT_TRUE(wait_on_task(driver, clock::now_ns() + LONG_DEADLINE_NS));

    EXPECT_LT(g_returned_at - g_called_at, SHORT_DEADLINE_NS);
    EXPECT_FALSE(driver->is_event_pending());
    heap::ufree_delete(driver);
}

// A deadline already behind the clock returns without waiting for an event
TEST(pci_driver_events, wait_until_past_deadline_returns_at_once) {
    auto* driver = heap::ualloc_new<event_test_driver>();
    ASSERT_NOT_NULL(driver);

    ASSERT_TRUE(wait_on_task(driver, clock::now_ns() - 1));

    EXPECT_LT(g_returned_at - g_called_at, SHORT_DEADLINE_NS);
    heap::ufree_delete(driver);
}
