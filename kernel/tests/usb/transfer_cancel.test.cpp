#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/usb/fake_xhci.h"

TEST_SUITE(usb_transfer_cancel);

using namespace usb_test;
using drivers::xhci::xhci_set_tr_dequeue_ptr_command_trb_t;
using drivers::xhci::xhci_stop_endpoint_command_trb_t;

static constexpr uint32_t TRANSFER_LENGTH   = 512;
static constexpr uint32_t BYTES_BEFORE_STOP = 64;
static constexpr uint8_t  DEVICE_BYTE       = 0x5A;
static constexpr uint8_t  SEND_BYTE         = 0xA5;

// A bulk OUT endpoint, whose DCI of 2 comes before the bulk IN endpoint's 3 in the step's scan
static constexpr uint8_t BULK_OUT = 0x01;

// Answers a stop with no transfer event, as a controller that never started the request may
static constexpr uint8_t NO_TRANSFER_EVENT = XHCI_TRB_COMPLETION_CODE_INVALID;

static constexpr uint32_t MAX_CANCELS = 3;
static constexpr uint32_t MAX_CALLS   = 4;

// The test runner, the HCD task's step and the cancels each run on a CPU of their own
static constexpr uint32_t MIN_CPUS = 3;

// One callback, as `record_call` records it
struct completion_call {
    usb::transfer_request* request;
    void*                  context;
    usb::transfer_status   status;
    uint32_t               actual_length;
    bool                   was_pending;
    bool                   was_elevated;
    sched::task*           task;
    uint32_t               doorbell;
};

// File scope, so a request the HCD task finishes after a failed test is never in a stack frame that has returned
static fake_xhci g_fake;
static usb::transfer_request g_first;
static usb::transfer_request g_second;
static usb::transfer_request g_third;
static uint8_t g_first_buffer[TRANSFER_LENGTH];
static uint8_t g_second_buffer[TRANSFER_LENGTH];
static uint8_t g_third_buffer[TRANSFER_LENGTH];
static sched::task* g_hcd_task;
static sync::atomic<uint32_t> g_hcd_task_done;
static sched::task* g_cancellers[MAX_CANCELS];
static uint32_t g_canceller_count;
static sync::atomic<uint32_t> g_cancels_done;
static uint8_t g_context;
static completion_call g_calls[MAX_CALLS];
static sync::atomic<uint32_t> g_call_count;

static int32_t attach_controller() {
    if (smp::cpu_count() < MIN_CPUS) {
        return -1;
    }

    string::memset(&g_fake, 0, sizeof(g_fake));
    string::memset(g_first_buffer, 0, sizeof(g_first_buffer));
    string::memset(g_second_buffer, 0, sizeof(g_second_buffer));
    string::memset(g_third_buffer, 0, sizeof(g_third_buffer));

    g_hcd_task = nullptr;
    g_canceller_count = 0;
    g_cancels_done.store_relaxed(0);
    string::memset(g_calls, 0, sizeof(g_calls));
    g_call_count.store_relaxed(0);

    return attach_fake_xhci(g_fake) ? 0 : -1;
}

// Raises interrupts until the HCD task's step ends, so a command the test left unanswered times out first, then ends
// what the test left in flight as an unplug does, so every cancel returns
static int32_t detach_controller() {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (g_hcd_task && !g_hcd_task_done.load_acquire() && clock::now_ns() < deadline) {
        raise_interrupt(g_fake);
        test_helpers::brief_delay();
    }

    bool is_step_running = g_hcd_task && !g_hcd_task_done.load_acquire();
    if (!is_step_running) {
        xhci_hcd_test_access::mark_unplugged(*g_fake.hcd, g_fake.device);
        xhci_hcd_test_access::end_unplugged_transfers(*g_fake.hcd, g_fake.device);
    }

    while (g_cancels_done.load_acquire() < g_canceller_count && clock::now_ns() < deadline) {
        test_helpers::brief_delay();
    }

    // The fake is left allocated while a task may still run on it, so no task wakes in freed memory
    if (is_step_running || g_cancels_done.load_acquire() < g_canceller_count) {
        return -1;
    }

    if (g_hcd_task) {
        test_helpers::unpin(g_hcd_task);
    }

    for (uint32_t i = 0; i < g_canceller_count; i++) {
        test_helpers::unpin(g_cancellers[i]);
    }

    detach_fake_xhci(g_fake);
    return 0;
}

BEFORE_EACH(usb_transfer_cancel, attach_controller);
AFTER_EACH(usb_transfer_cancel, detach_controller);

// Runs the step, then marks it done and ends its task
static void run_stop_step(void*) {
    xhci_hcd_test_access::stop_requested_endpoints(*g_fake.hcd);
    g_hcd_task_done.store_release(1);
    sched::exit(0);
}

// Runs the HCD task's step for cancelled transfers on a task of its own, lowered as the HCD task is, since its
// commands wait for the test
static bool start_stop_step() {
    g_hcd_task_done.store_relaxed(0);
    g_hcd_task = test_helpers::start_pinned_task_on_another_cpu(run_stop_step, nullptr, "usb_cancel_step", 0);
    return g_hcd_task != nullptr;
}

static void run_event_pass(void*) {
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    g_hcd_task_done.store_release(1);
    sched::exit(0);
}

// A pass that the driver takes for the HCD task's own, so a cancel made in its callbacks is one on the HCD task
static void run_event_pass_as_hcd_task(void*) {
    xhci_hcd_test_access::set_task(*g_fake.hcd, sched::current());
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    xhci_hcd_test_access::set_task(*g_fake.hcd, nullptr);
    g_hcd_task_done.store_release(1);
    sched::exit(0);
}

// Runs the HCD task's pass over the events on a task of its own, so the test runner goes on while a callback is held
static bool start_event_pass(void (*pass)(void*) = run_event_pass) {
    g_hcd_task_done.store_relaxed(0);
    g_hcd_task = test_helpers::start_pinned_task_on_another_cpu(pass, nullptr, "usb_event_pass", 0);
    return g_hcd_task != nullptr;
}

static void run_cancel(void* request) {
    usb::cancel_transfer(&g_fake.usb_device, *static_cast<usb::transfer_request*>(request));
    g_cancels_done.fetch_add_release(1);
    sched::exit(0);
}

// Cancels run two CPUs after their starter's, so a cancel that returns too early shows while the HCD task's step,
// one CPU after the test runner's, holds a callback
static constexpr uint32_t CANCEL_CPU_OFFSET = 2;

// Starts a cancel of `request` on a task of its own, lowered as a class driver's task is, since a cancel waits for the
// request's callback and the test runner must not wait
static sched::task* start_cancel(usb::transfer_request& request) {
    if (g_canceller_count == MAX_CANCELS) {
        return nullptr;
    }

    sched::task* canceller = nullptr;
    RUN_ELEVATED({
        uint32_t cpu = (percpu::current_cpu_id() + CANCEL_CPU_OFFSET) % smp::cpu_count();
        canceller = sched::create_kernel_task(run_cancel, &request, "usb_cancel", 0);
        if (canceller) {
            canceller->add_ref();
            sched::enqueue_on(canceller, cpu);
        }
    });

    if (canceller) {
        g_cancellers[g_canceller_count++] = canceller;
    }

    return canceller;
}

// Starts a cancel of `request` and gives whether it waits, as a cancel of a request in flight does. Its task's state
// is privileged and read elevated, since a callback on the HCD task's step starts cancels lowered
static bool start_waiting_cancel(usb::transfer_request& request) {
    sched::task* canceller = start_cancel(request);
    bool is_waiting = false;
    RUN_ELEVATED(is_waiting = canceller && test_helpers::blocks_before_deadline(canceller));
    return is_waiting;
}

// Records each callback with the request's fields, the privilege and task it ran with, and the slot's doorbell, which
// the test may clear first
static void record_call(void* context, usb::transfer_request& request) {
    uint32_t index = g_call_count.load_relaxed();
    if (index < MAX_CALLS) {
        completion_call& call = g_calls[index];
        call.request = &request;
        call.context = context;
        call.status = request.status;
        call.actual_length = request.actual_length;

        call.was_pending = request.pending;
        call.was_elevated = dynpriv::is_elevated();
        call.task = sched::current();
        call.doorbell = g_fake.doorbells[fake_xhci::SLOT_ID];
    }

    g_call_count.store_release(index + 1);
}

// Submits a bulk IN read of `TRANSFER_LENGTH` bytes with `usb::allow_short` and gives whether the driver accepted it
static bool submit_read(
    usb::transfer_request& request,
    uint8_t* buffer,
    usb::completion_callback on_complete = nullptr
) {
    usb::init_transfer_request(request, fake_xhci::BULK_IN, buffer, TRANSFER_LENGTH, usb::allow_short);
    usb::set_completion_callback(request, on_complete, &g_context);
    return usb::submit_transfer_async(&g_fake.usb_device, request) == 0;
}

// Submits a bulk OUT send of `TRANSFER_LENGTH` bytes and gives whether the driver accepted it
static bool submit_send(
    usb::transfer_request& request,
    uint8_t* buffer,
    usb::completion_callback on_complete = nullptr
) {
    usb::init_transfer_request(request, BULK_OUT, buffer, TRANSFER_LENGTH);
    usb::set_completion_callback(request, on_complete, &g_context);
    return usb::submit_transfer_async(&g_fake.usb_device, request) == 0;
}

static int32_t status_of(const usb::transfer_request& request) {
    return static_cast<int32_t>(request.status);
}

// Answers the Stop Endpoint command for `endpoint`, the bulk IN endpoint unless given, first posting a transfer event
// with `event_code` for the request at ring entry `entry` unless that is `NO_TRANSFER_EVENT`
static void answer_stop(uint8_t event_code, uint32_t residual, uint32_t entry = 0, xhci_endpoint* endpoint = nullptr) {
    xhci_endpoint* stopped = endpoint ? endpoint : g_fake.endpoint;
    auto* stop = reinterpret_cast<const xhci_stop_endpoint_command_trb_t*>(wait_for_command(g_fake, STOP_COMMAND));
    ASSERT_NOT_NULL(stop);
    EXPECT_EQ(stop->trb_type, static_cast<uint32_t>(XHCI_TRB_TYPE_STOP_ENDPOINT_CMD));
    EXPECT_EQ(stop->slot_id, fake_xhci::SLOT_ID);
    EXPECT_EQ(stop->endpoint_id, stopped->dci());

    if (event_code != NO_TRANSFER_EVENT) {
        post_transfer_event(g_fake, entry, residual, event_code, stopped);
    }

    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);
}

// Reads the endpoint's async flag under its lock, where the HCD task writes it
static bool is_async_enabled() {
    bool is_enabled = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_fake.endpoint->completion_lock());
        is_enabled = g_fake.endpoint->async_state()->async_enabled;
        sync::spin_unlock_irqrestore(g_fake.endpoint->completion_lock(), irq);
    });

    return is_enabled;
}

// A send the device stopped taking ends with the bytes the device took, and its buffer keeps what the caller wrote
TEST(usb_transfer_cancel, a_cancelled_send_reports_the_bytes_the_device_took) {
    xhci_endpoint* bulk_out = add_fake_endpoint(g_fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    ASSERT_NOT_NULL(bulk_out);
    string::memset(g_first_buffer, SEND_BYTE, sizeof(g_first_buffer));
    ASSERT_TRUE(submit_send(g_first, g_first_buffer));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH - BYTES_BEFORE_STOP, 0, bulk_out);
    answer_move(g_fake, 1, FIRST_LAP_CYCLE, bulk_out);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_first.actual_length, BYTES_BEFORE_STOP);
    EXPECT_EQ(g_first_buffer[0], SEND_BYTE);
    EXPECT_EQ(g_first_buffer[TRANSFER_LENGTH - 1], SEND_BYTE);
}

// The request finishes as cancelled, its buffer holding the bytes the controller reports moving before the stop, and
// the endpoint stays on the async path until its dequeue pointer has moved
TEST(usb_transfer_cancel, a_cancel_stops_the_endpoint_and_moves_its_dequeue_pointer_past_the_request) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_NOT_NULL(transfer_trb(g_fake, 0));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());

    string::memset(g_fake.endpoint->dma_buffer(), DEVICE_BYTE, BYTES_BEFORE_STOP);
    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH - BYTES_BEFORE_STOP);
    ASSERT_NOT_NULL(wait_for_command(g_fake, MOVE_COMMAND));
    EXPECT_TRUE(is_async_enabled());
    answer_move(g_fake);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_FALSE(g_first.pending);
    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_first.actual_length, BYTES_BEFORE_STOP);
    EXPECT_EQ(g_first_buffer[BYTES_BEFORE_STOP - 1], DEVICE_BYTE);
    EXPECT_EQ(g_first_buffer[BYTES_BEFORE_STOP], 0u);
}

// A request stopped with the invalid length code reports no bytes, whatever the event's length field holds
TEST(usb_transfer_cancel, a_stop_with_an_invalid_length_reports_no_bytes) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());

    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED_LENGTH_INVALID, 0);
    answer_move(g_fake);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_first.actual_length, 0u);
}

TEST(usb_transfer_cancel, a_stop_without_a_transfer_event_still_finishes_the_request) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());

    answer_stop(NO_TRANSFER_EVENT, 0);
    answer_move(g_fake);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_FALSE(g_first.pending);
    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_first.actual_length, 0u);
}

// The last ring entry before the Link TRB, and the cycle bit the ring's next lap writes
static constexpr uint32_t LAST_ENTRY     = XHCI_TRANSFER_RING_TRB_COUNT - 2;
static constexpr uint64_t NEXT_LAP_CYCLE = 0;

// The request takes the last entry, so the dequeue pointer moves to entry 0 on the ring's next lap
TEST(usb_transfer_cancel, a_stop_at_the_rings_end_moves_the_dequeue_pointer_to_the_next_lap) {
    xhci_trb_t filler = {};
    for (uint32_t i = 0; i < LAST_ENTRY; i++) {
        g_fake.endpoint->ring()->enqueue(&filler);
    }

    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());

    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH, LAST_ENTRY);
    answer_move(g_fake, 0, NEXT_LAP_CYCLE);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::cancelled));
}

// The second request, submitted while the stopped one is on the ring, and the third, submitted during the dequeue
// pointer's move, get no TRB or doorbell until the move completes, and then start one at a time
TEST(usb_transfer_cancel, requests_submitted_during_the_stop_start_after_the_dequeue_pointer_moves) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    g_fake.doorbells[fake_xhci::SLOT_ID] = 0;

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    ASSERT_NOT_NULL(wait_for_command(g_fake, STOP_COMMAND));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer));

    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH);
    ASSERT_NOT_NULL(wait_for_command(g_fake, MOVE_COMMAND));
    ASSERT_TRUE(submit_read(g_third, g_third_buffer));
    EXPECT_NULL(transfer_trb(g_fake, 1));
    EXPECT_EQ(g_fake.doorbells[fake_xhci::SLOT_ID], 0u);

    answer_move(g_fake);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_NOT_NULL(transfer_trb(g_fake, 1));
    EXPECT_NULL(transfer_trb(g_fake, 2));
    EXPECT_EQ(g_fake.doorbells[fake_xhci::SLOT_ID], static_cast<uint32_t>(g_fake.endpoint->dci()));
    EXPECT_TRUE(g_second.pending);
    EXPECT_TRUE(g_third.pending);
}

// The OUT endpoint's stop and dequeue move follow the IN endpoint's on the command ring
static constexpr uint32_t SECOND_STOP_COMMAND = 2;
static constexpr uint32_t SECOND_MOVE_COMMAND = 3;

// The OUT request is cancelled after the step's scan has passed the OUT endpoint, while the step stops the IN endpoint
TEST(usb_transfer_cancel, a_cancel_made_during_a_stop_stops_its_endpoint_in_the_same_step) {
    xhci_endpoint* bulk_out = add_fake_endpoint(g_fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    ASSERT_NOT_NULL(bulk_out);
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_TRUE(submit_send(g_second, g_second_buffer));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    ASSERT_NOT_NULL(wait_for_command(g_fake, STOP_COMMAND));

    ASSERT_TRUE(start_waiting_cancel(g_second));
    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH);
    answer_move(g_fake);

    auto* stop = reinterpret_cast<const xhci_stop_endpoint_command_trb_t*>(
        wait_for_command(g_fake, SECOND_STOP_COMMAND)
    );
    ASSERT_NOT_NULL(stop);
    EXPECT_EQ(stop->endpoint_id, bulk_out->dci());
    complete_command(g_fake, SECOND_STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    auto* move = reinterpret_cast<const xhci_set_tr_dequeue_ptr_command_trb_t*>(
        wait_for_command(g_fake, SECOND_MOVE_COMMAND)
    );
    ASSERT_NOT_NULL(move);
    EXPECT_EQ(move->new_dequeue_ptr, transfer_trb_phys(g_fake, 1, bulk_out) | FIRST_LAP_CYCLE);
    complete_command(g_fake, SECOND_MOVE_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    EXPECT_EQ(status_of(g_second), code_of(usb::transfer_status::cancelled));
}

// The request failed before the HCD task acted on the cancel, so the failure is what it reports
TEST(usb_transfer_cancel, a_request_that_failed_before_the_stop_keeps_its_error) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    post_transfer_event(g_fake, 0, TRANSFER_LENGTH, XHCI_TRB_COMPLETION_CODE_USB_TRANSACTION_ERROR);
    xhci_hcd_test_access::process_events(*g_fake.hcd);

    ASSERT_TRUE(start_stop_step());
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::io_error));
}

// The controller finished the request before the HCD task acted on the cancel, so the endpoint is never stopped
TEST(usb_transfer_cancel, a_request_completed_before_the_stop_keeps_its_own_status) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    string::memset(g_fake.endpoint->dma_buffer(), DEVICE_BYTE, BYTES_BEFORE_STOP);
    post_transfer_event(g_fake, 0, TRANSFER_LENGTH - BYTES_BEFORE_STOP, XHCI_TRB_COMPLETION_CODE_SHORT_PACKET);
    xhci_hcd_test_access::process_events(*g_fake.hcd);

    ASSERT_TRUE(start_stop_step());
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::short_packet));
    EXPECT_EQ(g_first.actual_length, BYTES_BEFORE_STOP);

    ASSERT_TRUE(submit_read(g_second, g_second_buffer));
    EXPECT_NOT_NULL(transfer_trb(g_fake, 1));
}

// The step's command waits clear the event the cancel marked pending, so the step marks it again for any work that
// arrived meanwhile
TEST(usb_transfer_cancel, a_cancel_and_its_stop_each_mark_the_hcd_tasks_event_pending) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    EXPECT_FALSE(xhci_hcd_test_access::is_event_pending(*g_fake.hcd));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    EXPECT_TRUE(xhci_hcd_test_access::is_event_pending(*g_fake.hcd));
    ASSERT_TRUE(start_stop_step());
    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH);

    ASSERT_NOT_NULL(wait_for_command(g_fake, MOVE_COMMAND));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(g_hcd_task));
    complete_command(g_fake, MOVE_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    EXPECT_TRUE(xhci_hcd_test_access::is_event_pending(*g_fake.hcd));
}

// Checks that the cancelled request and the one queued behind it failed, that nothing more reached the ring or the
// doorbell, and that the endpoint refuses a new request
static void expect_endpoint_retired() {
    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::io_error));
    EXPECT_EQ(status_of(g_second), code_of(usb::transfer_status::io_error));
    EXPECT_NULL(transfer_trb(g_fake, 1));
    EXPECT_EQ(g_fake.doorbells[fake_xhci::SLOT_ID], 0u);
    EXPECT_FALSE(submit_read(g_third, g_third_buffer));
}

// Queues a second request behind the cancelled one once the step has sent its stop
static void queue_behind_the_stop() {
    ASSERT_NOT_NULL(wait_for_command(g_fake, STOP_COMMAND));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer));
    g_fake.doorbells[fake_xhci::SLOT_ID] = 0;
}

// A controller refuses a stop when, for one, the endpoint halted on an error while the cancel waited
TEST(usb_transfer_cancel, a_refused_stop_fails_the_requests_and_retires_the_endpoint) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    queue_behind_the_stop();

    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    expect_endpoint_retired();
}

// The endpoint stopped with no transfer event, so its dequeue pointer, which the controller refused to move, is still
// at the request
TEST(usb_transfer_cancel, a_refused_dequeue_move_fails_the_requests_and_retires_the_endpoint) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    queue_behind_the_stop();

    answer_stop(NO_TRANSFER_EVENT, 0);
    complete_command(g_fake, MOVE_COMMAND, XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    expect_endpoint_retired();
}

static constexpr uint32_t SHORT_READ_BYTES = 20;

// The read's callback has returned, so the cancel has nothing to wait for and stops nothing
TEST(usb_transfer_cancel, a_cancel_after_the_callback_has_returned_does_not_wait) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer, record_call));
    post_transfer_event(g_fake, 0, TRANSFER_LENGTH - SHORT_READ_BYTES, XHCI_TRB_COMPLETION_CODE_SHORT_PACKET);
    xhci_hcd_test_access::process_events(*g_fake.hcd);

    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(g_calls[0].request, &g_first);
    EXPECT_EQ(g_calls[0].context, &g_context);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::short_packet));
    EXPECT_EQ(g_calls[0].actual_length, SHORT_READ_BYTES);
    EXPECT_FALSE(g_calls[0].was_pending);

    ASSERT_NOT_NULL(start_cancel(g_first));
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));
    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
}

static sync::atomic<uint32_t> g_await_done;
static int32_t g_await_result;

// Waits for the first request as a synchronous transfer does
static void run_await(void*) {
    g_await_result = usb::await_transfer(g_first);
    g_await_done.store_release(1);
    sched::exit(0);
}

TEST(usb_transfer_cancel, a_request_without_a_callback_wakes_its_waiter) {
    g_await_done.store_relaxed(0);
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    sched::task* waiter = test_helpers::start_pinned_task_on_another_cpu(run_await, nullptr, "usb_await", 0);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));

    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    ASSERT_TRUE(test_helpers::spin_wait(g_await_done));
    EXPECT_EQ(g_await_result, code_of(usb::transfer_status::ok));
    test_helpers::unpin(waiter);
}

static sync::atomic<uint32_t> g_is_call_held;
static sync::atomic<uint32_t> g_is_call_released;

// Records the call and holds it until the test releases it
static void hold_call(void* context, usb::transfer_request& request) {
    record_call(context, request);
    g_is_call_held.store_release(1);
    (void)test_helpers::spin_wait(g_is_call_released);
}

// The second read, cancelled while it waits behind the first, ends right after the first on the HCD task without
// reaching the ring, and its cancel waits for its callback. The third read then starts
TEST(usb_transfer_cancel, a_request_cancelled_in_the_queue_ends_in_its_turn_on_the_hcd_task) {
    g_is_call_held.store_relaxed(0);
    g_is_call_released.store_relaxed(0);
    ASSERT_TRUE(submit_read(g_first, g_first_buffer, record_call));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer, hold_call));
    ASSERT_TRUE(submit_read(g_third, g_third_buffer, record_call));
    ASSERT_TRUE(start_waiting_cancel(g_second));

    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    ASSERT_TRUE(start_event_pass());
    ASSERT_TRUE(test_helpers::spin_wait(g_is_call_held));
    test_helpers::brief_delay();
    EXPECT_EQ(g_cancels_done.load_acquire(), 0u);

    g_is_call_released.store_release(1);
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    ASSERT_EQ(g_call_count.load_acquire(), 2u);
    EXPECT_EQ(g_calls[0].request, &g_first);
    EXPECT_EQ(g_calls[1].request, &g_second);
    EXPECT_EQ(code_of(g_calls[1].status), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_calls[1].task, g_hcd_task);

    EXPECT_EQ(g_fake.endpoint->async_state()->active_request, &g_third);
    EXPECT_NULL(transfer_trb(g_fake, 2));
}

static usb::transfer_request g_send;
static uint8_t g_send_buffer[TRANSFER_LENGTH];

// The pass that completes the first read ends the cancelled second and starts the third, then completes a send. The
// third read's doorbell rings only as the pass ends, after the send's call
TEST(usb_transfer_cancel, a_request_started_after_a_cancelled_one_rings_once_the_pass_ends) {
    xhci_endpoint* bulk_out = add_fake_endpoint(g_fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    ASSERT_NOT_NULL(bulk_out);
    ASSERT_TRUE(submit_read(g_first, g_first_buffer, record_call));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer, record_call));
    ASSERT_TRUE(submit_read(g_third, g_third_buffer, record_call));
    ASSERT_TRUE(submit_send(g_send, g_send_buffer, record_call));

    ASSERT_TRUE(start_waiting_cancel(g_second));
    g_fake.doorbells[fake_xhci::SLOT_ID] = 0;

    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS, bulk_out);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));

    ASSERT_EQ(g_call_count.load_acquire(), 3u);
    EXPECT_EQ(code_of(g_calls[1].status), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_calls[2].request, &g_send);
    EXPECT_EQ(g_calls[2].doorbell, 0u);

    EXPECT_EQ(g_fake.endpoint->async_state()->active_request, &g_third);
    EXPECT_NOT_NULL(transfer_trb(g_fake, 1));
    EXPECT_EQ(g_fake.doorbells[fake_xhci::SLOT_ID], static_cast<uint32_t>(g_fake.endpoint->dci()));
}

static int32_t g_resubmit_result;

// Records the call, and submits the request again from a call that reports it cancelled
static void submit_again_when_cancelled(void* context, usb::transfer_request& request) {
    record_call(context, request);
    if (request.status == usb::transfer_status::cancelled) {
        g_resubmit_result = usb::submit_transfer_async(&g_fake.usb_device, request);
    }
}

// The second read's callback submits it again while the third waits, so the third starts, and the second queues behind
// it and starts in its turn, no longer cancelled
TEST(usb_transfer_cancel, a_request_submitted_while_others_wait_queues_behind_them) {
    g_resubmit_result = -1;
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer, submit_again_when_cancelled));
    ASSERT_TRUE(submit_read(g_third, g_third_buffer));
    ASSERT_TRUE(start_waiting_cancel(g_second));

    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));

    auto* state = g_fake.endpoint->async_state();
    EXPECT_EQ(g_resubmit_result, 0);
    EXPECT_EQ(state->active_request, &g_third);
    EXPECT_EQ(state->pending_head, &g_second);

    post_transfer_event(g_fake, 1, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    EXPECT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(state->active_request, &g_second);
}

// The stopped send's callback runs lowered on the HCD task's step, and the cancel waits until it returns
TEST(usb_transfer_cancel, a_cancel_returns_only_after_the_callback_of_the_send_it_stopped) {
    g_is_call_held.store_relaxed(0);
    g_is_call_released.store_relaxed(0);
    xhci_endpoint* bulk_out = add_fake_endpoint(g_fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    ASSERT_NOT_NULL(bulk_out);
    ASSERT_TRUE(submit_send(g_first, g_first_buffer, hold_call));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH - BYTES_BEFORE_STOP, 0, bulk_out);
    ASSERT_TRUE(test_helpers::spin_wait(g_is_call_held));
    test_helpers::brief_delay();
    EXPECT_EQ(g_cancels_done.load_acquire(), 0u);

    g_is_call_released.store_release(1);
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));
    answer_move(g_fake, 1, FIRST_LAP_CYCLE, bulk_out);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    ASSERT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_calls[0].actual_length, BYTES_BEFORE_STOP);
    EXPECT_FALSE(g_calls[0].was_elevated);
    EXPECT_EQ(g_calls[0].task, g_hcd_task);
}

// Records the call, and from the first call submits the request again, as a driver with more bytes to send would
static void submit_again_from_first_call(void* context, usb::transfer_request& request) {
    record_call(context, request);
    if (g_call_count.load_acquire() == 1) {
        g_resubmit_result = usb::submit_transfer_async(&g_fake.usb_device, request);
    }
}

// The cancel waits for the stopped send's callback and not for the request, which the callback submits again and the
// device never finishes
TEST(usb_transfer_cancel, a_cancel_returns_after_its_callback_though_the_send_is_submitted_again) {
    g_resubmit_result = -1;
    xhci_endpoint* bulk_out = add_fake_endpoint(g_fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    ASSERT_NOT_NULL(bulk_out);
    ASSERT_TRUE(submit_send(g_first, g_first_buffer, submit_again_from_first_call));

    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED, TRANSFER_LENGTH, 0, bulk_out);
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));
    answer_move(g_fake, 1, FIRST_LAP_CYCLE, bulk_out);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_EQ(g_resubmit_result, 0);
    EXPECT_EQ(g_call_count.load_acquire(), 1u);
    EXPECT_TRUE(g_first.pending);
    EXPECT_NOT_NULL(transfer_trb(g_fake, 1, bulk_out));
}

static uint32_t g_waiting_cancels;

// Records the call, then starts a cancel of its request and counts the cancel when it waits for the call to return
static void cancel_during_call(void* context, usb::transfer_request& request) {
    record_call(context, request);
    if (start_waiting_cancel(request)) {
        g_waiting_cancels++;
    }
}

// The read completes on its own, so the cancel finds it already off the endpoint and waits for its callback
TEST(usb_transfer_cancel, a_cancel_during_a_completed_requests_callback_waits_for_it) {
    g_waiting_cancels = 0;
    ASSERT_TRUE(submit_read(g_first, g_first_buffer, cancel_during_call));

    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    EXPECT_EQ(g_waiting_cancels, 1u);
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));
    EXPECT_EQ(g_fake.command_ring->get_virtual_base()[STOP_COMMAND].control, 0u);
}

// The second read ends cancelled once the first completes, in the HCD task's pass over the events
TEST(usb_transfer_cancel, a_cancel_during_a_cancelled_queued_requests_callback_waits_for_it) {
    g_waiting_cancels = 0;
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer, cancel_during_call));
    ASSERT_TRUE(start_waiting_cancel(g_second));

    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    xhci_hcd_test_access::process_events(*g_fake.hcd);
    EXPECT_EQ(g_waiting_cancels, 1u);
    ASSERT_TRUE(test_helpers::spin_wait_ge(g_cancels_done, 2));
}

// The controller answers the stop with no transfer event, so the step ends the read itself
TEST(usb_transfer_cancel, a_cancel_during_a_stopped_requests_callback_waits_for_it) {
    g_waiting_cancels = 0;
    ASSERT_TRUE(submit_read(g_first, g_first_buffer, cancel_during_call));
    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());

    answer_stop(NO_TRANSFER_EVENT, 0);
    answer_move(g_fake);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    EXPECT_EQ(g_waiting_cancels, 1u);
    ASSERT_TRUE(test_helpers::spin_wait_ge(g_cancels_done, 2));
}

// The controller refuses the stop, so the read queued behind the stopped one fails with it
TEST(usb_transfer_cancel, a_cancel_during_a_failed_queued_requests_callback_waits_for_it) {
    g_waiting_cancels = 0;
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));
    ASSERT_TRUE(start_waiting_cancel(g_first));
    ASSERT_TRUE(start_stop_step());
    ASSERT_NOT_NULL(wait_for_command(g_fake, STOP_COMMAND));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer, cancel_during_call));

    complete_command(g_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    EXPECT_EQ(g_waiting_cancels, 1u);
    ASSERT_TRUE(test_helpers::spin_wait_ge(g_cancels_done, 2));
}

// The unplug ends the active read and the two queued behind it in submission order, so a cancel during the last
// callback waits for it
TEST(usb_transfer_cancel, a_cancel_during_an_unplugs_last_callback_waits_for_it) {
    g_waiting_cancels = 0;
    ASSERT_TRUE(submit_read(g_first, g_first_buffer, record_call));
    ASSERT_TRUE(submit_read(g_second, g_second_buffer, record_call));
    ASSERT_TRUE(submit_read(g_third, g_third_buffer, cancel_during_call));

    xhci_hcd_test_access::mark_unplugged(*g_fake.hcd, g_fake.device);
    xhci_hcd_test_access::end_unplugged_transfers(*g_fake.hcd, g_fake.device);
    EXPECT_EQ(g_waiting_cancels, 1u);
    ASSERT_TRUE(test_helpers::spin_wait(g_cancels_done));

    ASSERT_EQ(g_call_count.load_acquire(), 3u);
    EXPECT_EQ(g_calls[0].request, &g_first);
    EXPECT_EQ(g_calls[1].request, &g_second);
    EXPECT_EQ(g_calls[2].request, &g_third);

    EXPECT_EQ(code_of(g_calls[0].status), code_of(usb::transfer_status::device_gone));
    EXPECT_EQ(code_of(g_calls[1].status), code_of(usb::transfer_status::device_gone));
    EXPECT_EQ(code_of(g_calls[2].status), code_of(usb::transfer_status::device_gone));
}

// The unplug refuses the submit, which leaves nothing on the endpoint for the unplug to call back
TEST(usb_transfer_cancel, a_submit_the_unplug_refuses_is_never_called_back) {
    xhci_hcd_test_access::mark_unplugged(*g_fake.hcd, g_fake.device);
    EXPECT_FALSE(submit_read(g_first, g_first_buffer, record_call));
    EXPECT_FALSE(g_first.pending);
    EXPECT_NULL(g_fake.endpoint->async_state()->active_request);
    EXPECT_NULL(transfer_trb(g_fake, 0));

    xhci_hcd_test_access::end_unplugged_transfers(*g_fake.hcd, g_fake.device);
    EXPECT_EQ(g_call_count.load_acquire(), 0u);
}

static sync::atomic<uint32_t> g_has_own_cancel_returned;

// Records the call, then cancels its own request, which a callback must not do
static void cancel_own_request(void* context, usb::transfer_request& request) {
    record_call(context, request);
    usb::cancel_transfer(&g_fake.usb_device, request);
    g_has_own_cancel_returned.store_release(1);
}

// The HCD task makes every callback, so its cancel of the request whose callback it is running returns without waiting
TEST(usb_transfer_cancel, a_cancel_on_the_hcd_task_returns_without_waiting) {
    g_has_own_cancel_returned.store_relaxed(0);
    ASSERT_TRUE(submit_read(g_first, g_first_buffer, cancel_own_request));

    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    ASSERT_TRUE(start_event_pass(run_event_pass_as_hcd_task));
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    EXPECT_EQ(g_has_own_cancel_returned.load_acquire(), 1u);
    EXPECT_EQ(g_call_count.load_acquire(), 1u);
}
