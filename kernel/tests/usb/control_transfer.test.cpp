#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/usb/fake_xhci.h"
#include "sync/atomic.h"

TEST_SUITE(usb_control_transfer);

using namespace usb_test;
using drivers::xhci::xhci_setup_stage_trb_t;
using test_helpers::blocks_before_deadline;
using test_helpers::spin_wait;
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::unpin;

// A vendor request that sends data to the device. Its bytes wait in the control buffer until the controller reads them
static constexpr uint8_t  VENDOR_OUT_REQUEST_TYPE = 0x40;
static constexpr uint8_t  VENDOR_REQUEST          = 0x01;
static constexpr uint16_t OUT_LENGTH              = 8;

// The bytes each of a test's two requests sends, which it also carries as its wValue
static constexpr uint8_t FIRST_BYTE  = 0xA1;
static constexpr uint8_t SECOND_BYTE = 0xB2;

// A request's TD takes three EP0 ring entries, its setup, data and status stages, so a test's two TDs start at 0 and 3
static constexpr uint32_t TD_TRBS   = 3;
static constexpr uint32_t FIRST_TD  = 0;
static constexpr uint32_t SECOND_TD = FIRST_TD + TD_TRBS;

// A recovery after a stall sends Reset Endpoint in the command ring entry a stop takes, then its dequeue move
static constexpr uint32_t RESET_COMMAND = STOP_COMMAND;

// A stop the controller refuses for a halt it raced is followed by Reset Endpoint and then the move
static constexpr uint32_t RESET_AFTER_STOP_COMMAND = 1;
static constexpr uint32_t MOVE_AFTER_RESET_COMMAND = 2;

// The timeout a test gives a request or command it lets time out, short so the suite stays fast
static constexpr uint64_t SHORT_TIMEOUT_NS = 50ULL * 1000 * 1000;

// A control transfer a task makes, with the request it sends and the result the test reads back
struct control_transfer_call {
    uint16_t               value;
    uint8_t                data[OUT_LENGTH];
    int32_t                rc;
    sync::atomic<uint32_t> is_done;
};

// The test's fake, on the unprivileged heap, so a test whose call never ends can leave it allocated for that call
static fake_xhci* s_fake;

// Builds a fake for the test. False when memory runs out
static bool attach_controller() {
    s_fake = static_cast<fake_xhci*>(heap::uzalloc(sizeof(fake_xhci)));
    if (!s_fake) {
        return false;
    }

    if (!attach_fake_xhci(*s_fake)) {
        detach_fake_xhci(*s_fake);
        heap::ufree(s_fake);
        return false;
    }

    return true;
}

// Frees the fake when `are_calls_done`, and otherwise leaks it so a call still waiting never reaches freed memory
static void detach_when_done(bool are_calls_done) {
    xhci_hcd_test_access::set_task(*s_fake->hcd, nullptr);
    if (are_calls_done) {
        detach_fake_xhci(*s_fake);
        heap::ufree(s_fake);
    }
}

// Readies `call` to send `OUT_LENGTH` copies of `byte`, with `byte` as its wValue
static void prepare_out_call(control_transfer_call& call, uint8_t byte) {
    call.value = byte;
    for (uint8_t& data_byte : call.data) {
        data_byte = byte;
    }

    call.rc = 0;
    call.is_done.store_release(0);
}

// Makes the call's request through `usb::control_transfer`, the entry class drivers use, and marks it done
static void make_call(control_transfer_call& call) {
    call.rc = usb::control_transfer(
        &s_fake->usb_device,
        VENDOR_OUT_REQUEST_TYPE,
        VENDOR_REQUEST,
        call.value,
        0,
        call.data,
        OUT_LENGTH
    );
    call.is_done.store_release(1);
}

// Makes the call from a task other than the HCD task, as a class driver's task does
static void run_driver_call(void* arg) {
    make_call(*static_cast<control_transfer_call*>(arg));
    sched::exit(0);
}

// Makes the call as the HCD task, as enumeration and class driver probes do
static void run_hcd_call(void* arg) {
    xhci_hcd_test_access::set_task(*s_fake->hcd, sched::current());
    make_call(*static_cast<control_transfer_call*>(arg));
    sched::exit(0);
}

// Starts `entry` making `call` on a lowered task, as class driver and HCD tasks run, pinned until `finish_call`
static sched::task* start_call(void (*entry)(void*), control_transfer_call& call) {
    return start_pinned_task_on_another_cpu(entry, &call, "usb_control_call", 0);
}

// Waits for the call as `spin_wait` does, unpins its task and returns whether the call is done
static bool finish_call(sched::task* task, control_transfer_call& call) {
    bool is_done = spin_wait(call.is_done);
    unpin(task);

    return is_done;
}

// True when the control buffer's first `OUT_LENGTH` bytes are all `byte`
static bool control_buffer_holds(uint8_t byte) {
    const uint8_t* buffer = static_cast<const uint8_t*>(s_fake->device->ctrl_transfer_buffer());
    for (uint16_t i = 0; i < OUT_LENGTH; i++) {
        if (buffer[i] != byte) {
            return false;
        }
    }

    return true;
}

// Waits for the TD the driver queues at EP0 ring entry `first` and checks it is the request sending `byte`, with the
// control buffer holding its bytes. False when no TD arrives there
static bool wait_for_td_sending(uint32_t first, uint8_t byte) {
    const xhci_trb_t* status = wait_for_control_trb(*s_fake, first + TD_TRBS - 1);
    if (!status) {
        return false;
    }

    auto* setup = reinterpret_cast<const xhci_setup_stage_trb_t*>(control_trb(*s_fake, first));
    uint16_t value = setup->request_packet.wValue;
    EXPECT_EQ(setup->request_packet.bRequestType, VENDOR_OUT_REQUEST_TYPE);
    EXPECT_EQ(value, static_cast<uint16_t>(byte));
    EXPECT_TRUE(control_buffer_holds(byte));

    return true;
}

// Ends the TD at EP0 ring entry `first` with `completion_code`, as the HCD task handles its event for a class driver
static void complete_driver_td(uint32_t first, uint8_t completion_code) {
    post_control_event(*s_fake, first + TD_TRBS - 1, completion_code);
    xhci_hcd_test_access::process_events(*s_fake->hcd);
}

// Ends the TD at EP0 ring entry `first` with `completion_code` for a request the HCD task makes, which handles the
// event itself once the interrupt wakes it
static void complete_hcd_td(uint32_t first, uint8_t completion_code) {
    post_control_event(*s_fake, first + TD_TRBS - 1, completion_code);
    raise_interrupt(*s_fake);
}

static control_transfer_call s_driver_calls[2];

// A second class driver request waits for EP0 without writing its bytes over the first request's, which the controller
// has yet to read
TEST(usb_control_transfer, a_request_waiting_for_ep0_leaves_the_control_buffer_to_the_request_holding_ep0) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_driver_calls[0], FIRST_BYTE);
    prepare_out_call(s_driver_calls[1], SECOND_BYTE);

    sched::task* first = start_call(run_driver_call, s_driver_calls[0]);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(wait_for_control_trb(*s_fake, FIRST_TD + TD_TRBS - 1));

    sched::task* second = start_call(run_driver_call, s_driver_calls[1]);
    ASSERT_NOT_NULL(second);
    ASSERT_TRUE(blocks_before_deadline(second));

    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    complete_driver_td(FIRST_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    EXPECT_TRUE(wait_for_td_sending(SECOND_TD, SECOND_BYTE));
    complete_driver_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    bool is_first_done = finish_call(first, s_driver_calls[0]);
    bool is_second_done = finish_call(second, s_driver_calls[1]);
    detach_when_done(is_first_done && is_second_done);
    EXPECT_EQ(s_driver_calls[0].rc, 0);
    EXPECT_EQ(s_driver_calls[1].rc, 0);
}

static control_transfer_call s_late_call;

// A class driver's request made once teardown has ended EP0's transfers fails without queuing a TD or waiting
TEST(usb_control_transfer, a_request_after_teardown_fails_without_queuing_a_td) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_late_call, FIRST_BYTE);
    xhci_hcd_test_access::end_control_transfers(*s_fake->hcd, s_fake->device);

    sched::task* late = start_call(run_driver_call, s_late_call);
    ASSERT_NOT_NULL(late);
    EXPECT_FALSE(blocks_before_deadline(late));

    // A request that waits instead of failing is ended as teardown ends one, so the test can free the fake
    xhci_hcd_test_access::end_control_transfers(*s_fake->hcd, s_fake->device);
    uint32_t doorbell = s_fake->doorbells[fake_xhci::SLOT_ID];
    bool is_td_queued = control_trb(*s_fake, FIRST_TD) != nullptr;

    detach_when_done(finish_call(late, s_late_call));
    EXPECT_EQ(s_late_call.rc, -1);
    EXPECT_EQ(doorbell, 0u);
    EXPECT_FALSE(is_td_queued);
}

static control_transfer_call s_waiting_call;

// A class driver's request waiting for its TD when teardown ends EP0's transfers fails
TEST(usb_control_transfer, a_request_waiting_at_teardown_fails) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_waiting_call, FIRST_BYTE);

    sched::task* waiting = start_call(run_driver_call, s_waiting_call);
    ASSERT_NOT_NULL(waiting);
    ASSERT_NOT_NULL(wait_for_control_trb(*s_fake, FIRST_TD + TD_TRBS - 1));
    ASSERT_TRUE(blocks_before_deadline(waiting));

    xhci_hcd_test_access::end_control_transfers(*s_fake->hcd, s_fake->device);
    detach_when_done(finish_call(waiting, s_waiting_call));
    EXPECT_EQ(s_waiting_call.rc, -1);
}

static control_transfer_call s_mixed_calls[2];

// While a class driver's request holds EP0 and waits for a completion only the HCD task handles, a request on the HCD
// task fails at once, without ringing EP0's doorbell or writing the control buffer
TEST(usb_control_transfer, a_request_on_the_hcd_task_fails_at_once_while_a_class_driver_holds_ep0) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_mixed_calls[0], FIRST_BYTE);
    prepare_out_call(s_mixed_calls[1], SECOND_BYTE);

    sched::task* driver = start_call(run_driver_call, s_mixed_calls[0]);
    ASSERT_NOT_NULL(driver);
    ASSERT_NOT_NULL(wait_for_control_trb(*s_fake, FIRST_TD + TD_TRBS - 1));

    // Takes the doorbell the driver rings for the first TD once it has rung, so a ring for a second TD shows
    ASSERT_TRUE(wait_for_doorbell(*s_fake, fake_xhci::SLOT_ID, CONTROL_DCI, test_helpers::SPIN_TIMEOUT_NS));
    sched::task* hcd = start_call(run_hcd_call, s_mixed_calls[1]);
    ASSERT_NOT_NULL(hcd);
    EXPECT_FALSE(blocks_before_deadline(hcd));
    uint32_t doorbell = s_fake->doorbells[fake_xhci::SLOT_ID];
    bool is_second_td_queued = control_trb(*s_fake, SECOND_TD) != nullptr;

    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    complete_driver_td(FIRST_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    // A request that waits instead of failing is answered as after an interrupt, so the test can free the fake
    if (!s_mixed_calls[1].is_done.load_acquire() && wait_for_td_sending(SECOND_TD, SECOND_BYTE)) {
        complete_hcd_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    }

    bool is_driver_done = finish_call(driver, s_mixed_calls[0]);
    bool is_hcd_done = finish_call(hcd, s_mixed_calls[1]);
    detach_when_done(is_driver_done && is_hcd_done);

    EXPECT_EQ(doorbell, 0u);
    EXPECT_FALSE(is_second_td_queued);
    EXPECT_EQ(s_mixed_calls[0].rc, 0);
    EXPECT_EQ(s_mixed_calls[1].rc, -1);
}

static sync::atomic<uint32_t> s_is_step_done;

// Runs the stop step as the HCD task, then marks the step done
static void run_recovery_step(void*) {
    xhci_hcd_test_access::set_task(*s_fake->hcd, sched::current());
    xhci_hcd_test_access::stop_requested_endpoints(*s_fake->hcd);
    s_is_step_done.store_release(1);
    sched::exit(0);
}

// Answers the Reset Endpoint command for EP0 in command ring entry `index`
static void answer_control_reset(uint32_t index) {
    auto* reset = reinterpret_cast<const drivers::xhci::xhci_reset_endpoint_command_trb_t*>(
        wait_for_command(*s_fake, index)
    );
    ASSERT_NOT_NULL(reset);

    EXPECT_EQ(reset->trb_type, static_cast<uint32_t>(XHCI_TRB_TYPE_RESET_ENDPOINT_CMD));
    EXPECT_EQ(reset->slot_id, fake_xhci::SLOT_ID);
    EXPECT_EQ(reset->endpoint_id, CONTROL_DCI);
    complete_command(*s_fake, index, XHCI_TRB_COMPLETION_CODE_SUCCESS);
}

static control_transfer_call s_stalled_call;
static control_transfer_call s_call_during_recovery;

// A stall on a class driver's request leaves EP0's recovery to the HCD task's stop step, which resets EP0, moves its
// dequeue pointer past the stalled TD and marks an event pending, while a second request waits for EP0 until then
TEST(usb_control_transfer, a_stalled_class_driver_request_has_the_hcd_task_recover_ep0) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_stalled_call, FIRST_BYTE);
    prepare_out_call(s_call_during_recovery, SECOND_BYTE);
    s_is_step_done.store_release(0);

    sched::task* stalled = start_call(run_driver_call, s_stalled_call);
    ASSERT_NOT_NULL(stalled);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    complete_driver_td(FIRST_TD, XHCI_TRB_COMPLETION_CODE_STALL_ERROR);
    ASSERT_TRUE(blocks_before_deadline(stalled));
    bool has_driver_sent_command = command_trb(*s_fake, RESET_COMMAND) != nullptr;

    // Clears the doorbell the stalled request rang, so a ring for the second request's TD shows
    s_fake->doorbells[fake_xhci::SLOT_ID] = 0;
    sched::task* next = start_call(run_driver_call, s_call_during_recovery);
    ASSERT_NOT_NULL(next);
    ASSERT_TRUE(blocks_before_deadline(next));
    uint32_t doorbell = s_fake->doorbells[fake_xhci::SLOT_ID];

    sched::task* step = start_pinned_task_on_another_cpu(run_recovery_step, nullptr, "usb_recovery_step", 0);
    ASSERT_NOT_NULL(step);
    answer_control_reset(RESET_COMMAND);

    // The step waits for its move with every earlier wake taken, so an event pending once it ends is the step's own
    ASSERT_NOT_NULL(wait_for_command(*s_fake, MOVE_COMMAND));
    ASSERT_TRUE(blocks_before_deadline(step));
    answer_control_move(*s_fake, SECOND_TD);
    EXPECT_TRUE(spin_wait(s_is_step_done));
    unpin(step);
    bool is_event_pending = xhci_hcd_test_access::is_event_pending(*s_fake->hcd);

    EXPECT_TRUE(wait_for_td_sending(SECOND_TD, SECOND_BYTE));
    complete_driver_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    bool is_stalled_done = finish_call(stalled, s_stalled_call);
    bool is_next_done = finish_call(next, s_call_during_recovery);
    detach_when_done(is_stalled_done && is_next_done);

    EXPECT_FALSE(has_driver_sent_command);
    EXPECT_EQ(doorbell, 0u);
    EXPECT_TRUE(is_event_pending);
    EXPECT_EQ(s_stalled_call.rc, -1);
    EXPECT_EQ(s_call_during_recovery.rc, 0);
}

static control_transfer_call s_recovery_waiter;

// A class driver's request waiting for EP0's recovery fails once teardown ends EP0's transfers, before any step runs
TEST(usb_control_transfer, a_request_waiting_for_ep0s_recovery_fails_at_teardown) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_recovery_waiter, FIRST_BYTE);

    sched::task* waiter = start_call(run_driver_call, s_recovery_waiter);
    ASSERT_NOT_NULL(waiter);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    complete_driver_td(FIRST_TD, XHCI_TRB_COMPLETION_CODE_STALL_ERROR);
    ASSERT_TRUE(blocks_before_deadline(waiter));

    xhci_hcd_test_access::end_control_transfers(*s_fake->hcd, s_fake->device);
    detach_when_done(finish_call(waiter, s_recovery_waiter));
    EXPECT_EQ(s_recovery_waiter.rc, -1);
}

// Waits until a class driver's request has asked the HCD task to recover EP0. False when `SPIN_TIMEOUT_NS` passes
static bool wait_for_recovery_request() {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (true) {
        bool is_requested = false;
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(s_fake->device->ctrl_completion_lock());
            is_requested = s_fake->device->ctrl_recovery_requested();
            sync::spin_unlock_irqrestore(s_fake->device->ctrl_completion_lock(), irq);
        });

        if (is_requested) {
            return true;
        }

        if (clock::now_ns() > deadline) {
            return false;
        }
    }
}

// Waits for the Stop Endpoint command for EP0 and checks it
static void expect_control_stop() {
    auto* stop = reinterpret_cast<const drivers::xhci::xhci_stop_endpoint_command_trb_t*>(
        wait_for_command(*s_fake, STOP_COMMAND)
    );
    ASSERT_NOT_NULL(stop);

    EXPECT_EQ(stop->trb_type, static_cast<uint32_t>(XHCI_TRB_TYPE_STOP_ENDPOINT_CMD));
    EXPECT_EQ(stop->slot_id, fake_xhci::SLOT_ID);
    EXPECT_EQ(stop->endpoint_id, CONTROL_DCI);
}

// Answers EP0's Stop Endpoint command as a controller does, with a Stopped event for the TD at EP0 ring entry `first`
// before the command's completion
static void answer_control_stop(uint32_t first) {
    expect_control_stop();
    post_control_event(*s_fake, first + TD_TRBS - 1, XHCI_TRB_COMPLETION_CODE_STOPPED);
    complete_command(*s_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_SUCCESS);
}

static control_transfer_call s_silent_call;
static control_transfer_call s_call_after_stop;

// A class driver's request whose TD never completes fails once its timeout passes, and the HCD task stops EP0 and
// moves its dequeue pointer past the TD, so the driver's next request runs and never collects the stopped TD's event
TEST(usb_control_transfer, a_class_driver_request_that_never_completes_fails_and_has_the_hcd_task_stop_ep0) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_silent_call, FIRST_BYTE);
    prepare_out_call(s_call_after_stop, SECOND_BYTE);
    s_is_step_done.store_release(0);
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);

    sched::task* silent = start_call(run_driver_call, s_silent_call);
    ASSERT_NOT_NULL(silent);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    ASSERT_TRUE(wait_for_recovery_request());
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, drivers::xhci::CONTROL_TRANSFER_TIMEOUT_NS);

    sched::task* step = start_pinned_task_on_another_cpu(run_recovery_step, nullptr, "usb_recovery_step", 0);
    ASSERT_NOT_NULL(step);
    answer_control_stop(FIRST_TD);
    answer_control_move(*s_fake, SECOND_TD);
    EXPECT_TRUE(spin_wait(s_is_step_done));
    unpin(step);

    sched::task* next = start_call(run_driver_call, s_call_after_stop);
    ASSERT_NOT_NULL(next);
    EXPECT_TRUE(wait_for_td_sending(SECOND_TD, SECOND_BYTE));
    complete_driver_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    bool is_silent_done = finish_call(silent, s_silent_call);
    bool is_next_done = finish_call(next, s_call_after_stop);
    detach_when_done(is_silent_done && is_next_done);
    EXPECT_EQ(s_silent_call.rc, -1);
    EXPECT_EQ(s_call_after_stop.rc, 0);
}

static control_transfer_call s_silent_hcd_call;
static control_transfer_call s_hcd_call_after_stop;

// A request on the HCD task whose TD never completes fails once its timeout passes with no event to wake the task,
// which stops EP0 and moves its dequeue pointer past the TD itself, so its next request runs
TEST(usb_control_transfer, a_request_on_the_hcd_task_that_never_completes_fails_and_stops_ep0) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_silent_hcd_call, FIRST_BYTE);
    prepare_out_call(s_hcd_call_after_stop, SECOND_BYTE);
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);

    sched::task* silent = start_call(run_hcd_call, s_silent_hcd_call);
    ASSERT_NOT_NULL(silent);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    answer_control_stop(FIRST_TD);

    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, drivers::xhci::CONTROL_TRANSFER_TIMEOUT_NS);
    answer_control_move(*s_fake, SECOND_TD);
    EXPECT_TRUE(spin_wait(s_silent_hcd_call.is_done));

    sched::task* next = start_call(run_hcd_call, s_hcd_call_after_stop);
    ASSERT_NOT_NULL(next);
    EXPECT_TRUE(wait_for_td_sending(SECOND_TD, SECOND_BYTE));
    complete_hcd_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    bool is_silent_done = finish_call(silent, s_silent_hcd_call);
    bool is_next_done = finish_call(next, s_hcd_call_after_stop);
    detach_when_done(is_silent_done && is_next_done);
    EXPECT_EQ(s_silent_hcd_call.rc, -1);
    EXPECT_EQ(s_hcd_call_after_stop.rc, 0);
}

static control_transfer_call s_timed_out_call;
static control_transfer_call s_call_after_late_td;

// When a TD completes after its class driver request timed out but before the HCD task's step, EP0 has run past it,
// so the step sends no command and the driver's next request runs
TEST(usb_control_transfer, a_td_completing_after_its_request_timed_out_needs_no_recovery) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_timed_out_call, FIRST_BYTE);
    prepare_out_call(s_call_after_late_td, SECOND_BYTE);
    s_is_step_done.store_release(0);
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);

    sched::task* timed_out = start_call(run_driver_call, s_timed_out_call);
    ASSERT_NOT_NULL(timed_out);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    ASSERT_TRUE(wait_for_recovery_request());
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, drivers::xhci::CONTROL_TRANSFER_TIMEOUT_NS);
    complete_driver_td(FIRST_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    sched::task* step = start_pinned_task_on_another_cpu(run_recovery_step, nullptr, "usb_recovery_step", 0);
    ASSERT_NOT_NULL(step);
    EXPECT_TRUE(spin_wait(s_is_step_done));
    unpin(step);
    bool has_step_sent_command = command_trb(*s_fake, STOP_COMMAND) != nullptr;

    sched::task* next = start_call(run_driver_call, s_call_after_late_td);
    ASSERT_NOT_NULL(next);
    EXPECT_TRUE(wait_for_td_sending(SECOND_TD, SECOND_BYTE));
    complete_driver_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    bool is_timed_out_done = finish_call(timed_out, s_timed_out_call);
    bool is_next_done = finish_call(next, s_call_after_late_td);
    detach_when_done(is_timed_out_done && is_next_done);
    EXPECT_FALSE(has_step_sent_command);
    EXPECT_EQ(s_timed_out_call.rc, -1);
    EXPECT_EQ(s_call_after_late_td.rc, 0);
}

static control_transfer_call s_raced_call;
static control_transfer_call s_call_after_reset;

// A halt that ends a timed-out class driver request before the step reads it makes the controller refuse the step's
// stop, so the step resets EP0 before the move and the driver's next request runs
TEST(usb_control_transfer, a_halt_that_races_the_stop_is_reset_before_the_move) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_raced_call, FIRST_BYTE);
    prepare_out_call(s_call_after_reset, SECOND_BYTE);
    s_is_step_done.store_release(0);
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);

    sched::task* raced = start_call(run_driver_call, s_raced_call);
    ASSERT_NOT_NULL(raced);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    ASSERT_TRUE(wait_for_recovery_request());
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, drivers::xhci::CONTROL_TRANSFER_TIMEOUT_NS);
    post_control_event(*s_fake, FIRST_TD + TD_TRBS - 1, XHCI_TRB_COMPLETION_CODE_STALL_ERROR);

    sched::task* step = start_pinned_task_on_another_cpu(run_recovery_step, nullptr, "usb_recovery_step", 0);
    ASSERT_NOT_NULL(step);
    expect_control_stop();
    complete_command(*s_fake, STOP_COMMAND, XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR);

    answer_control_reset(RESET_AFTER_STOP_COMMAND);
    answer_control_move(*s_fake, SECOND_TD, MOVE_AFTER_RESET_COMMAND);
    EXPECT_TRUE(spin_wait(s_is_step_done));
    unpin(step);

    sched::task* next = start_call(run_driver_call, s_call_after_reset);
    ASSERT_NOT_NULL(next);
    EXPECT_TRUE(wait_for_td_sending(SECOND_TD, SECOND_BYTE));
    complete_driver_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    bool is_raced_done = finish_call(raced, s_raced_call);
    bool is_next_done = finish_call(next, s_call_after_reset);
    detach_when_done(is_raced_done && is_next_done);
    EXPECT_EQ(s_raced_call.rc, -1);
    EXPECT_EQ(s_call_after_reset.rc, 0);
}

static control_transfer_call s_unmoved_call;
static control_transfer_call s_driver_call_after_failure;
static control_transfer_call s_hcd_call_after_failure;

// A move the controller refuses leaves the stalled TD on EP0's ring, where it could still run, so the stalled request
// fails and every later request, a class driver's or the HCD task's, fails at once without ringing EP0's doorbell
TEST(usb_control_transfer, a_move_the_controller_refuses_fails_every_later_request_at_once) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_unmoved_call, FIRST_BYTE);
    prepare_out_call(s_driver_call_after_failure, SECOND_BYTE);
    prepare_out_call(s_hcd_call_after_failure, SECOND_BYTE);
    s_is_step_done.store_release(0);

    sched::task* unmoved = start_call(run_driver_call, s_unmoved_call);
    ASSERT_NOT_NULL(unmoved);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    complete_driver_td(FIRST_TD, XHCI_TRB_COMPLETION_CODE_STALL_ERROR);
    ASSERT_TRUE(wait_for_recovery_request());

    sched::task* step = start_pinned_task_on_another_cpu(run_recovery_step, nullptr, "usb_recovery_step", 0);
    ASSERT_NOT_NULL(step);
    answer_control_reset(RESET_COMMAND);
    complete_command(*s_fake, MOVE_COMMAND, XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR);
    EXPECT_TRUE(spin_wait(s_is_step_done));
    unpin(step);

    // Clears the doorbell the stalled request rang, so a ring for a later request's TD shows
    s_fake->doorbells[fake_xhci::SLOT_ID] = 0;
    sched::task* driver = start_call(run_driver_call, s_driver_call_after_failure);
    ASSERT_NOT_NULL(driver);
    EXPECT_FALSE(blocks_before_deadline(driver));

    sched::task* hcd = start_call(run_hcd_call, s_hcd_call_after_failure);
    ASSERT_NOT_NULL(hcd);
    EXPECT_FALSE(blocks_before_deadline(hcd));
    uint32_t doorbell = s_fake->doorbells[fake_xhci::SLOT_ID];
    bool is_td_queued = control_trb(*s_fake, SECOND_TD) != nullptr;

    bool is_unmoved_done = finish_call(unmoved, s_unmoved_call);
    bool is_driver_done = finish_call(driver, s_driver_call_after_failure);
    bool is_hcd_done = finish_call(hcd, s_hcd_call_after_failure);
    detach_when_done(is_unmoved_done && is_driver_done && is_hcd_done);

    EXPECT_EQ(doorbell, 0u);
    EXPECT_FALSE(is_td_queued);
    EXPECT_EQ(s_unmoved_call.rc, -1);
    EXPECT_EQ(s_driver_call_after_failure.rc, -1);
    EXPECT_EQ(s_hcd_call_after_failure.rc, -1);
}

static control_transfer_call s_unstopped_call;
static control_transfer_call s_call_after_unstopped;

// A stop the controller never answers fails once the command timeout passes, and the step sends no move behind it, so
// the timed-out request fails and EP0 takes no later request
TEST(usb_control_transfer, a_stop_the_controller_never_answers_fails_ep0_without_a_move) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_unstopped_call, FIRST_BYTE);
    prepare_out_call(s_call_after_unstopped, SECOND_BYTE);
    s_is_step_done.store_release(0);
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);
    xhci_hcd_test_access::set_command_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);

    sched::task* unstopped = start_call(run_driver_call, s_unstopped_call);
    ASSERT_NOT_NULL(unstopped);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    ASSERT_TRUE(wait_for_recovery_request());

    sched::task* step = start_pinned_task_on_another_cpu(run_recovery_step, nullptr, "usb_recovery_step", 0);
    ASSERT_NOT_NULL(step);
    expect_control_stop();
    EXPECT_TRUE(spin_wait(s_is_step_done));
    unpin(step);
    bool has_step_sent_move = command_trb(*s_fake, MOVE_COMMAND) != nullptr;

    // Clears the doorbell the timed-out request rang, so a ring for the next request's TD shows
    s_fake->doorbells[fake_xhci::SLOT_ID] = 0;
    sched::task* next = start_call(run_driver_call, s_call_after_unstopped);
    ASSERT_NOT_NULL(next);
    EXPECT_FALSE(blocks_before_deadline(next));
    uint32_t doorbell = s_fake->doorbells[fake_xhci::SLOT_ID];

    bool is_unstopped_done = finish_call(unstopped, s_unstopped_call);
    bool is_next_done = finish_call(next, s_call_after_unstopped);
    detach_when_done(is_unstopped_done && is_next_done);

    EXPECT_FALSE(has_step_sent_move);
    EXPECT_EQ(doorbell, 0u);
    EXPECT_EQ(s_unstopped_call.rc, -1);
    EXPECT_EQ(s_call_after_unstopped.rc, -1);
}

static control_transfer_call s_late_success_call;
static control_transfer_call s_call_after_late_success;

// A TD that completes while the step's stop goes unanswered left EP0 running past it, so the step sends no move and
// fails nothing, and the driver's next request runs
TEST(usb_control_transfer, a_td_completing_during_an_unanswered_stop_needs_no_move) {
    ASSERT_TRUE(attach_controller());
    prepare_out_call(s_late_success_call, FIRST_BYTE);
    prepare_out_call(s_call_after_late_success, SECOND_BYTE);
    s_is_step_done.store_release(0);
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);
    xhci_hcd_test_access::set_command_timeout(*s_fake->hcd, SHORT_TIMEOUT_NS);

    sched::task* timed_out = start_call(run_driver_call, s_late_success_call);
    ASSERT_NOT_NULL(timed_out);
    EXPECT_TRUE(wait_for_td_sending(FIRST_TD, FIRST_BYTE));
    ASSERT_TRUE(wait_for_recovery_request());
    xhci_hcd_test_access::set_control_transfer_timeout(*s_fake->hcd, drivers::xhci::CONTROL_TRANSFER_TIMEOUT_NS);
    post_control_event(*s_fake, FIRST_TD + TD_TRBS - 1, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    sched::task* step = start_pinned_task_on_another_cpu(run_recovery_step, nullptr, "usb_recovery_step", 0);
    ASSERT_NOT_NULL(step);
    expect_control_stop();
    EXPECT_TRUE(spin_wait(s_is_step_done));
    unpin(step);
    bool has_step_sent_move = command_trb(*s_fake, MOVE_COMMAND) != nullptr;

    sched::task* next = start_call(run_driver_call, s_call_after_late_success);
    ASSERT_NOT_NULL(next);
    EXPECT_TRUE(wait_for_td_sending(SECOND_TD, SECOND_BYTE));
    complete_driver_td(SECOND_TD, XHCI_TRB_COMPLETION_CODE_SUCCESS);

    bool is_timed_out_done = finish_call(timed_out, s_late_success_call);
    bool is_next_done = finish_call(next, s_call_after_late_success);
    detach_when_done(is_timed_out_done && is_next_done);
    EXPECT_FALSE(has_step_sent_move);
    EXPECT_EQ(s_late_success_call.rc, -1);
    EXPECT_EQ(s_call_after_late_success.rc, 0);
}
