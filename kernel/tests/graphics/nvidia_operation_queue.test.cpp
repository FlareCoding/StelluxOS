#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "clock/clock.h"
#include "drivers/device_driver.h"
#include "drivers/graphics/nvidia/operation_queue.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/task.h"

TEST_SUITE(nvidia_operation_queue);

using drivers::nvidia::operation;
using drivers::nvidia::operation_queue;
using test_helpers::spin_wait;

constexpr uint32_t ORDER_MAX = 4;

// A driver whose task a test plays, by calling `serve` itself
class queue_test_driver : public drivers::device_driver {
public:
    queue_test_driver() : device_driver("queue_test") {}

    int32_t attach() override { return 0; }
    void run() override {}

    bool is_event_pending() const { return m_event_pending; }
};

// An operation whose work records its number in `g_order` and sets its result to it
struct numbered_operation : operation {
    uint32_t number;
};

// A caller in another context, its task and what its wait for `waited` returned
struct waiting_caller {
    operation_queue*       queue;
    numbered_operation*    waited;
    sched::task*           task;
    int32_t                result;
    sync::atomic<uint32_t> has_returned;
};

static uint32_t g_order[ORDER_MAX];
static uint32_t g_order_count;

static void record_number(operation& op) {
    auto& numbered = static_cast<numbered_operation&>(op);
    if (g_order_count < ORDER_MAX) {
        g_order[g_order_count] = numbered.number;
    }

    g_order_count++;
    op.result = static_cast<int32_t>(numbered.number);
}

static numbered_operation numbered(uint32_t number) {
    numbered_operation op = {};
    op.work = record_number;
    op.number = number;

    return op;
}

// Posted operations run in the order they were queued, each post marking the driver's event pending
TEST(nvidia_operation_queue, serve_runs_posted_operations_in_arrival_order) {
    auto* driver = heap::ualloc_new<queue_test_driver>();
    ASSERT_NOT_NULL(driver);
    auto* queue = heap::ualloc_new<operation_queue>(*driver);
    ASSERT_NOT_NULL(queue);
    g_order_count = 0;

    numbered_operation first = numbered(1);
    numbered_operation second = numbered(2);
    numbered_operation third = numbered(3);
    RUN_ELEVATED({
        queue->post_to_task(first);
        queue->post_to_task(second);
        queue->post_to_task(third);
    });
    EXPECT_TRUE(driver->is_event_pending());
    EXPECT_EQ(g_order_count, 0u);

    queue->serve();
    ASSERT_EQ(g_order_count, 3u);
    EXPECT_EQ(g_order[0], 1u);
    EXPECT_EQ(g_order[1], 2u);
    EXPECT_EQ(g_order[2], 3u);

    queue->serve();
    EXPECT_EQ(g_order_count, 3u);
    heap::ufree_delete(queue);
    heap::ufree_delete(driver);
}

static operation_queue* g_requeue_queue;
static numbered_operation g_requeued;

// Records its number, then posts `g_requeued` from inside its own work
static void record_and_post(operation& op) {
    record_number(op);
    RUN_ELEVATED(g_requeue_queue->post_to_task(g_requeued));
}

// An operation queued while the task serves runs in the same serve, after the one that queued it
TEST(nvidia_operation_queue, serve_runs_operations_queued_while_it_serves) {
    auto* driver = heap::ualloc_new<queue_test_driver>();
    ASSERT_NOT_NULL(driver);
    auto* queue = heap::ualloc_new<operation_queue>(*driver);
    ASSERT_NOT_NULL(queue);

    g_order_count = 0;
    g_requeue_queue = queue;
    g_requeued = numbered(2);
    numbered_operation first = numbered(1);
    first.work = record_and_post;
    RUN_ELEVATED(queue->post_to_task(first));

    queue->serve();
    ASSERT_EQ(g_order_count, 2u);
    EXPECT_EQ(g_order[0], 1u);
    EXPECT_EQ(g_order[1], 2u);
    heap::ufree_delete(queue);
    heap::ufree_delete(driver);
}

// Records its number, then frees its own record, as a buffer's release frees the buffer holding it
static void record_and_free(operation& op) {
    record_number(op);
    heap::ufree_delete(static_cast<numbered_operation*>(&op));
}

// A posted operation's work may free its record, and the operations behind it still run
TEST(nvidia_operation_queue, a_posted_operation_may_free_its_record) {
    auto* driver = heap::ualloc_new<queue_test_driver>();
    ASSERT_NOT_NULL(driver);
    auto* queue = heap::ualloc_new<operation_queue>(*driver);
    ASSERT_NOT_NULL(queue);
    g_order_count = 0;

    auto* freed = heap::ualloc_new<numbered_operation>();
    ASSERT_NOT_NULL(freed);
    *freed = numbered(1);
    freed->work = record_and_free;
    numbered_operation behind = numbered(2);
    RUN_ELEVATED({
        queue->post_to_task(*freed);
        queue->post_to_task(behind);
    });

    queue->serve();
    ASSERT_EQ(g_order_count, 2u);
    EXPECT_EQ(g_order[0], 1u);
    EXPECT_EQ(g_order[1], 2u);
    heap::ufree_delete(queue);
    heap::ufree_delete(driver);
}

// Runs the caller's operation through its queue, then ends its task
static void wait_for_operation(void* arg) {
    auto* caller = static_cast<waiting_caller*>(arg);
    RUN_ELEVATED(caller->result = caller->queue->run_on_task(*caller->waited));
    caller->has_returned.store_release(1);
    sched::exit(0);
}

// Starts `caller`'s task waiting for `waited` and gives whether it sleeps before the deadline
static bool start_waiting(waiting_caller& caller, operation_queue& queue, numbered_operation& waited) {
    caller.queue = &queue;
    caller.waited = &waited;
    caller.result = 0;
    caller.has_returned.store_relaxed(0);
    caller.task = test_helpers::start_pinned_task(wait_for_operation, &caller, "operation_wait");

    return caller.task != nullptr && test_helpers::blocks_before_deadline(caller.task);
}

static waiting_caller g_caller;

// A caller sleeps until the task serves its operation, then returns the result the work set
TEST(nvidia_operation_queue, run_on_task_returns_the_result_once_served) {
    auto* driver = heap::ualloc_new<queue_test_driver>();
    ASSERT_NOT_NULL(driver);
    auto* queue = heap::ualloc_new<operation_queue>(*driver);
    ASSERT_NOT_NULL(queue);
    auto* waited = heap::ualloc_new<numbered_operation>();
    ASSERT_NOT_NULL(waited);

    *waited = numbered(7);
    g_order_count = 0;
    ASSERT_TRUE(start_waiting(g_caller, *queue, *waited));
    EXPECT_TRUE(driver->is_event_pending());
    EXPECT_EQ(g_caller.has_returned.load_acquire(), 0u);

    queue->serve();
    ASSERT_TRUE(spin_wait(g_caller.has_returned));
    EXPECT_EQ(g_order_count, 1u);
    EXPECT_EQ(g_caller.result, 7);
    EXPECT_TRUE(waited->is_done);

    test_helpers::unpin(g_caller.task);
    heap::ufree_delete(waited);
    heap::ufree_delete(queue);
    heap::ufree_delete(driver);
}

static waiting_caller g_first_caller;
static waiting_caller g_second_caller;
static bool g_has_second_caller_slept_on;

// Records its number once the second caller, which the first operation's wake woke too, sleeps again or returns
static void record_once_the_second_caller_settles(operation& op) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    bool has_settled = false;
    while (!has_settled && clock::now_ns() < deadline) {
        bool is_asleep = g_second_caller.task->state.load_acquire() == sched::TASK_STATE_BLOCKED;
        has_settled = is_asleep || g_second_caller.has_returned.load_acquire() != 0;
    }

    g_has_second_caller_slept_on = has_settled && g_second_caller.has_returned.load_acquire() == 0;
    record_number(op);
}

// The wake ending one caller's wait wakes every waiting caller, in whatever order they wait, and one whose operation is
// still queued sleeps again until the task has run it
TEST(nvidia_operation_queue, a_caller_sleeps_through_the_wake_ending_another_callers_wait) {
    auto* driver = heap::ualloc_new<queue_test_driver>();
    ASSERT_NOT_NULL(driver);
    auto* queue = heap::ualloc_new<operation_queue>(*driver);
    ASSERT_NOT_NULL(queue);

    auto* first = heap::ualloc_new<numbered_operation>();
    ASSERT_NOT_NULL(first);
    auto* second = heap::ualloc_new<numbered_operation>();
    ASSERT_NOT_NULL(second);

    *first = numbered(1);
    *second = numbered(2);
    second->work = record_once_the_second_caller_settles;

    g_order_count = 0;
    g_has_second_caller_slept_on = false;
    ASSERT_TRUE(start_waiting(g_first_caller, *queue, *first));
    ASSERT_TRUE(start_waiting(g_second_caller, *queue, *second));
    RUN_ELEVATED(sched::wake(g_first_caller.task));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(g_first_caller.task));

    queue->serve();
    ASSERT_TRUE(spin_wait(g_first_caller.has_returned));
    ASSERT_TRUE(spin_wait(g_second_caller.has_returned));
    EXPECT_TRUE(g_has_second_caller_slept_on);
    EXPECT_EQ(g_first_caller.result, 1);
    EXPECT_EQ(g_second_caller.result, 2);

    test_helpers::unpin(g_first_caller.task);
    test_helpers::unpin(g_second_caller.task);
    heap::ufree_delete(second);
    heap::ufree_delete(first);
    heap::ufree_delete(queue);
    heap::ufree_delete(driver);
}
