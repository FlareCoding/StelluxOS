#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/usb/fake_xhci.h"
#include "drivers/usb/core/usb_core.h"

TEST_SUITE(xhci_waits);

using namespace usb_test;

// A request the setup test expects the fake controller to serve
struct expected_request {
    served_kind kind;
    uint8_t     code;
};

// The interrupt IN endpoint whose stream reads during the driver's waits, opened without a callback as HID opens it
static constexpr uint8_t  INTERRUPT_IN         = 0x83;
static constexpr uint8_t  INTERRUPT_ATTRIBUTES = 3;
static constexpr uint16_t INTERRUPT_MAX_PACKET = 8;

// How long the fake controller watches for the read to be posted again before it completes what the driver waits for
static constexpr uint64_t REPOST_TIMEOUT_NS = 1000000000ULL;

// The dequeue pointer's bits in the interrupter's ERDP register, above the segment index and the busy flag
static constexpr uint64_t ERDP_POINTER_MASK = ~static_cast<uint64_t>(0xF);

// File scope, so a step the driver task ends after a failed test never writes into a stack frame that has returned
static fake_xhci g_fake;
static xhci_endpoint* g_interrupt_in;

// Opens the stream, whose first read rings the endpoint's doorbell, then clears the doorbell so the next ring shows
static int32_t attach_controller() {
    string::memset(&g_fake, 0, sizeof(g_fake));
    if (!attach_fake_xhci(g_fake)) {
        return -1;
    }

    g_interrupt_in = add_fake_endpoint(g_fake, INTERRUPT_IN, INTERRUPT_ATTRIBUTES, INTERRUPT_MAX_PACKET);
    if (!g_interrupt_in) {
        return -1;
    }

    int32_t rc = g_fake.hcd->usb_open_in_stream(g_fake.device, INTERRUPT_IN, INTERRUPT_MAX_PACKET, 1, nullptr, nullptr);
    g_fake.doorbells[fake_xhci::SLOT_ID] = 0;

    return rc;
}

// A step still running keeps the fake, which is then never freed, so the step never wakes in freed memory
static int32_t detach_controller() {
    if (g_fake.step_task && !finish_step(g_fake)) {
        return -1;
    }

    detach_fake_xhci(g_fake);
    return 0;
}

BEFORE_EACH(xhci_waits, attach_controller);
AFTER_EACH(xhci_waits, detach_controller);

// Completes the stream's posted read and reports whether the driver posts it again within `timeout_ns`
static bool complete_read_and_watch_for_repost(uint64_t timeout_ns) {
    post_transfer_event(g_fake, 0, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS, g_interrupt_in);
    raise_interrupt(g_fake);

    return wait_for_doorbell(g_fake, fake_xhci::SLOT_ID, g_interrupt_in->dci(), timeout_ns);
}

// Sends a No Op command and waits for its completion, recording what the wait returned
static void send_no_op_command(fake_xhci& fake) {
    xhci_trb_t no_op = XHCI_CONSTRUCT_CMD_TRB(XHCI_TRB_TYPE_NOOP_CMD);
    fake.step_result = xhci_hcd_test_access::send_command(*fake.hcd, &no_op);
}

// The driver must also move the dequeue pointer past both events. Only the read's pass rings a doorbell, so the
// doorbell stays idle once the fake controller has taken that ring
TEST(xhci_waits, a_read_completed_during_a_command_is_posted_again_before_the_command_completes) {
    ASSERT_TRUE(start_step(g_fake, send_no_op_command));

    bool is_command_queued = wait_for_command(g_fake, 0) != nullptr;
    EXPECT_TRUE(is_command_queued);

    bool is_reposted = false;
    if (is_command_queued) {
        is_reposted = complete_read_and_watch_for_repost(REPOST_TIMEOUT_NS);
        complete_command(g_fake, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS);
    }

    ASSERT_TRUE(finish_step(g_fake));
    EXPECT_TRUE(is_reposted);
    EXPECT_EQ(g_fake.step_result, 0);
    EXPECT_EQ(g_fake.doorbells[fake_xhci::SLOT_ID], 0u);

    uint64_t dequeue_pointer = g_fake.interrupter.erdp & ERDP_POINTER_MASK;
    EXPECT_EQ(dequeue_pointer, g_fake.event_ring->get_physical_base() + g_fake.next_event * sizeof(xhci_trb_t));
}

// Selects configuration 1 of the fake's device and waits for the transfer, recording what it returned
static void set_configuration(fake_xhci& fake) {
    fake.step_result = fake.hcd->usb_control_transfer(
        fake.device,
        usb::USB_REQTYPE_DIR_OUT,
        usb::USB_REQUEST_SET_CONFIGURATION,
        1,
        0,
        nullptr,
        0
    );
}

// Only the read's pass rings a doorbell, so the doorbell stays idle once the fake controller has taken that ring
TEST(xhci_waits, a_read_completed_during_a_control_transfer_is_posted_again_before_the_transfer_completes) {
    ASSERT_TRUE(start_step(g_fake, set_configuration));

    xhci_device_request_packet request = {};
    bool is_transfer_queued = take_control_transfer(g_fake, g_fake.device, &request);
    EXPECT_TRUE(is_transfer_queued);

    bool is_reposted = false;
    if (is_transfer_queued) {
        is_reposted = complete_read_and_watch_for_repost(REPOST_TIMEOUT_NS);
        complete_control_transfer(g_fake, g_fake.device, nullptr, 0);
    }

    ASSERT_TRUE(finish_step(g_fake));
    EXPECT_TRUE(is_reposted);
    EXPECT_EQ(g_fake.step_result, 0);
    EXPECT_EQ(g_fake.doorbells[fake_xhci::SLOT_ID], 0u);
}

static constexpr uint8_t RESET_PORT_INDEX = 1;

// When `reset_root_port` began on the driver's task, which opens the power-on wait's window
static uint64_t g_reset_start_ns;

// Memory keeps the 0 that acknowledging the reset writes to the write-1-to-clear enable bit, so the reset fails
static void reset_root_port(fake_xhci& fake) {
    g_reset_start_ns = clock::now_ns();
    (void)xhci_hcd_test_access::reset_port(*fake.hcd, RESET_PORT_INDEX);
}

TEST(xhci_waits, a_read_completed_during_a_port_reset_is_posted_again_before_the_reset_finishes) {
    drivers::xhci::xhci_portsc_register connected = {};
    connected.ccs = 1;
    connected.pp = 1;
    *portsc(g_fake, RESET_PORT_INDEX) = connected.raw;
    ASSERT_TRUE(start_step(g_fake, reset_root_port));

    bool is_reset_started = wait_for_port_reset(g_fake, RESET_PORT_INDEX);
    EXPECT_TRUE(is_reset_started);

    bool is_reposted = false;
    if (is_reset_started) {
        is_reposted = complete_read_and_watch_for_repost(REPOST_TIMEOUT_NS);
        complete_port_reset(g_fake, RESET_PORT_INDEX);
    }

    ASSERT_TRUE(finish_step(g_fake));
    EXPECT_TRUE(is_reposted);
}

// The driver's power-on wait for an unpowered port, and its two settles after a reset
static constexpr uint64_t POWER_ON_NS = 20000000;
static constexpr uint64_t SETTLES_NS  = 6000000;

// A busy wait charges the driver's task for the power-on wait or a settle, and a busy poll never blocks
TEST(xhci_waits, a_port_reset_sleeps_through_its_power_on_wait_polls_and_settles) {
    drivers::xhci::xhci_portsc_register unpowered = {};
    unpowered.ccs = 1;
    *portsc(g_fake, RESET_PORT_INDEX) = unpowered.raw;
    ASSERT_TRUE(start_step(g_fake, reset_root_port));

    uint64_t polling_cpu_ns = 0;
    bool is_polling_asleep = wait_for_port_reset(g_fake, RESET_PORT_INDEX) &&
                             read_blocked_step_cpu_ns(g_fake, &polling_cpu_ns);
    EXPECT_TRUE(is_polling_asleep);

    uint64_t reset_end_ns = clock::now_ns();
    complete_port_reset(g_fake, RESET_PORT_INDEX);

    ASSERT_TRUE(finish_step(g_fake));
    EXPECT_GE(reset_end_ns - g_reset_start_ns - polling_cpu_ns, POWER_ON_NS * 3 / 4);
    EXPECT_GE(g_fake.step_end_ns - reset_end_ns, SETTLES_NS);
    EXPECT_GE(g_fake.step_end_ns - reset_end_ns - (g_fake.step_cpu_ns - polling_cpu_ns), SETTLES_NS * 3 / 4);
}

// The hub port the setup test sets a device up on, of the fake's device acting as a hub of four ports, and the slot
// the fake controller gives that device
static constexpr uint8_t SETUP_HUB_PORT = 3;
static constexpr uint8_t HUB_PORT_COUNT = 4;
static constexpr uint8_t SETUP_SLOT_ID  = 2;

// Each of setup's pauses: the SET_ADDRESS recovery and the pause before each descriptor retry
static constexpr uint64_t SETUP_PAUSE_NS = 10000000;

// What setup sends when the device answers each descriptor's first read implausibly
static constexpr expected_request SETUP_REQUESTS[] = {
    {served_kind::command, XHCI_TRB_TYPE_ENABLE_SLOT_CMD},
    {served_kind::command, XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD},
    {served_kind::control_transfer, usb::USB_REQUEST_GET_DESCRIPTOR},
    {served_kind::control_transfer, usb::USB_REQUEST_GET_DESCRIPTOR},
    {served_kind::command, XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD},
    {served_kind::control_transfer, usb::USB_REQUEST_GET_DESCRIPTOR},
    {served_kind::control_transfer, usb::USB_REQUEST_GET_DESCRIPTOR},
    {served_kind::control_transfer, usb::USB_REQUEST_GET_DESCRIPTOR},
    {served_kind::control_transfer, usb::USB_REQUEST_GET_DESCRIPTOR},
    {served_kind::control_transfer, usb::USB_REQUEST_GET_DESCRIPTOR},
    {served_kind::command, XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD},
    {served_kind::control_transfer, usb::USB_REQUEST_SET_CONFIGURATION},
};

static constexpr size_t SETUP_REQUEST_COUNT = sizeof(SETUP_REQUESTS) / sizeof(SETUP_REQUESTS[0]);

// The request each pause ends at: the retry of the first 8 bytes, the whole descriptor after SET_ADDRESS's recovery,
// and the retries of the whole descriptor and the configuration header
static constexpr size_t PAUSE_ENDS[] = {3, 5, 6, 8};

// Sets up a high-speed device on the hub port, as the hub driver's report of a connection does
static void set_up_hub_port(fake_xhci& fake) {
    xhci_hcd_test_access::setup_hub_device(*fake.hcd, fake.device, SETUP_HUB_PORT, XHCI_USB_SPEED_HIGH_SPEED);
}

// A busy pause charges the driver's task for the whole pause, while a sleep charges it for almost none of it
TEST(xhci_waits, device_setup_sleeps_through_the_set_address_recovery_and_its_retry_pauses) {
    g_fake.device->set_is_hub(true);
    g_fake.device->set_hub_num_ports(HUB_PORT_COUNT);
    g_fake.should_answer_first_reads_implausibly = true;
    ASSERT_TRUE(start_step(g_fake, set_up_hub_port));

    served_log served = {};
    ASSERT_TRUE(serve_until_step_done(g_fake, SETUP_SLOT_ID, served));
    ASSERT_EQ(served.count, SETUP_REQUEST_COUNT);

    for (size_t i = 0; i < SETUP_REQUEST_COUNT; i++) {
        EXPECT_EQ(served.requests[i].kind, SETUP_REQUESTS[i].kind);
        EXPECT_EQ(served.requests[i].code, SETUP_REQUESTS[i].code);
    }

    for (size_t end : PAUSE_ENDS) {
        uint64_t waited_ns = served.requests[end].at_ns - served.requests[end - 1].at_ns;
        uint64_t ran_ns = served.requests[end].cpu_ns - served.requests[end - 1].cpu_ns;
        EXPECT_GE(waited_ns, SETUP_PAUSE_NS);
        EXPECT_GE(waited_ns - ran_ns, SETUP_PAUSE_NS * 3 / 4);
    }

    xhci_device* new_device = g_fake.device->hub_child(SETUP_HUB_PORT);
    ASSERT_NOT_NULL(new_device);
    EXPECT_NOT_NULL(new_device->core_device());

    g_fake.device->set_hub_child(SETUP_HUB_PORT, nullptr);
    xhci_hcd_test_access::forget_slot_device(*g_fake.hcd, new_device);
    usb::core::device_disconnected(g_fake.hcd, new_device);
    usb::core::finalize_disconnected_device(g_fake.hcd, new_device);
}

// How long `wait_long` waits, twice the time the test gives the read to be posted again
static constexpr uint64_t LONG_WAIT_US = 1000000;

static sync::atomic<uint32_t> g_is_long_wait_started;
static uint64_t g_long_wait_ns;

// Marks the wait started, then waits `LONG_WAIT_US` on the driver and records how long the wait took
static void wait_long(fake_xhci& fake) {
    uint64_t start_ns = clock::now_ns();
    g_is_long_wait_started.store_release(1);
    xhci_hcd_test_access::wait_us_handling_events(*fake.hcd, LONG_WAIT_US);
    g_long_wait_ns = clock::now_ns() - start_ns;
}

// The read completes in the wait's first half and must be posted again before that half ends
TEST(xhci_waits, a_wait_handles_events_as_they_arrive_and_lasts_its_whole_duration) {
    g_is_long_wait_started.store_relaxed(0);
    ASSERT_TRUE(start_step(g_fake, wait_long));

    bool is_wait_started = test_helpers::spin_wait(g_is_long_wait_started);
    EXPECT_TRUE(is_wait_started);

    bool is_reposted = false;
    if (is_wait_started) {
        is_reposted = complete_read_and_watch_for_repost(LONG_WAIT_US * 1000 / 2);
    }

    ASSERT_TRUE(finish_step(g_fake));
    EXPECT_TRUE(is_reposted);
    EXPECT_GE(g_long_wait_ns, LONG_WAIT_US * 1000);
}
