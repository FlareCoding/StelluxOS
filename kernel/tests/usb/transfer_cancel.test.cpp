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

static int32_t attach_controller() {
    string::memset(&g_fake, 0, sizeof(g_fake));
    string::memset(g_first_buffer, 0, sizeof(g_first_buffer));
    string::memset(g_second_buffer, 0, sizeof(g_second_buffer));
    string::memset(g_third_buffer, 0, sizeof(g_third_buffer));
    g_hcd_task = nullptr;
    return attach_fake_xhci(g_fake) ? 0 : -1;
}

// Raises interrupts until the HCD task's step ends, so a command the test left unanswered times out first
static int32_t detach_controller() {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (g_hcd_task && !g_hcd_task_done.load_acquire() && clock::now_ns() < deadline) {
        raise_interrupt(g_fake);
        test_helpers::brief_delay();
    }

    if (g_hcd_task) {
        test_helpers::unpin(g_hcd_task);
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

// Submits a bulk IN read of `TRANSFER_LENGTH` bytes with `usb::allow_short` and gives whether the driver accepted it
static bool submit_read(usb::transfer_request& request, uint8_t* buffer) {
    usb::init_transfer_request(request, fake_xhci::BULK_IN, buffer, TRANSFER_LENGTH, usb::allow_short);
    return usb::submit_transfer_async(&g_fake.usb_device, request) == 0;
}

// Submits a bulk OUT send of `TRANSFER_LENGTH` bytes and gives whether the driver accepted it
static bool submit_send(usb::transfer_request& request, uint8_t* buffer) {
    usb::init_transfer_request(request, BULK_OUT, buffer, TRANSFER_LENGTH);
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

    usb::cancel_transfer(&g_fake.usb_device, g_first);
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

    usb::cancel_transfer(&g_fake.usb_device, g_first);
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

    usb::cancel_transfer(&g_fake.usb_device, g_first);
    ASSERT_TRUE(start_stop_step());

    answer_stop(XHCI_TRB_COMPLETION_CODE_STOPPED_LENGTH_INVALID, 0);
    answer_move(g_fake);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::cancelled));
    EXPECT_EQ(g_first.actual_length, 0u);
}

TEST(usb_transfer_cancel, a_stop_without_a_transfer_event_still_finishes_the_request) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));

    usb::cancel_transfer(&g_fake.usb_device, g_first);
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
    usb::cancel_transfer(&g_fake.usb_device, g_first);
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

    usb::cancel_transfer(&g_fake.usb_device, g_first);
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

    usb::cancel_transfer(&g_fake.usb_device, g_first);
    ASSERT_TRUE(start_stop_step());
    ASSERT_NOT_NULL(wait_for_command(g_fake, STOP_COMMAND));

    usb::cancel_transfer(&g_fake.usb_device, g_second);
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

    usb::cancel_transfer(&g_fake.usb_device, g_first);
    post_transfer_event(g_fake, 0, TRANSFER_LENGTH, XHCI_TRB_COMPLETION_CODE_USB_TRANSACTION_ERROR);
    xhci_hcd_test_access::process_events(*g_fake.hcd);

    ASSERT_TRUE(start_stop_step());
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));

    EXPECT_EQ(status_of(g_first), code_of(usb::transfer_status::io_error));
}

// The controller finished the request before the HCD task acted on the cancel, so the endpoint is never stopped
TEST(usb_transfer_cancel, a_request_completed_before_the_stop_keeps_its_own_status) {
    ASSERT_TRUE(submit_read(g_first, g_first_buffer));

    usb::cancel_transfer(&g_fake.usb_device, g_first);
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

    usb::cancel_transfer(&g_fake.usb_device, g_first);
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
    usb::cancel_transfer(&g_fake.usb_device, g_first);
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
    usb::cancel_transfer(&g_fake.usb_device, g_first);
    ASSERT_TRUE(start_stop_step());
    queue_behind_the_stop();

    answer_stop(NO_TRANSFER_EVENT, 0);
    complete_command(g_fake, MOVE_COMMAND, XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR);
    ASSERT_TRUE(test_helpers::spin_wait(g_hcd_task_done));
    expect_endpoint_retired();
}
