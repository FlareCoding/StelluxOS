#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/usb/fake_xhci.h"

TEST_SUITE(xhci_waits);

using namespace usb_test;

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
