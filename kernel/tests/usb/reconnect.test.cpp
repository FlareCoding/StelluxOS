#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/usb/fake_xhci.h"
#include "drivers/usb/core/usb_core.h"
#include "drivers/usb/hub/hub_driver.h"

namespace usb::hub {

// Gives the hub test access to the private members of `hub_driver`, which declares it a friend
struct hub_driver_test_access {
    static void handle_port_change(hub_driver& hub, uint8_t port) {
        hub.handle_port_change(port);
    }
};

} // namespace usb::hub

TEST_SUITE(usb_reconnect);

using namespace usb_test;

// What the hub the fake's device plays reports for the port the hub driver tests change, and how the port changes as
// the driver handles it
struct hub_port_model {
    uint16_t status;
    uint16_t change;
    uint32_t status_reads;
    bool     is_device_back_after_first_read;
    bool     is_second_read_failed;
};

// The slot of the device recorded before the reconnect, and the slot the fake controller hands the new device
static constexpr uint8_t OLD_SLOT = 2;
static constexpr uint8_t NEW_SLOT = 3;

static constexpr uint8_t ROOT_PORT_INDEX = 2;

// The port the hub tests reconnect, on the fake's device acting as a hub of four ports
static constexpr uint8_t HUB_PORT       = 3;
static constexpr uint8_t HUB_PORT_COUNT = 4;

// File scope, so a step the driver task ends after a failed test never writes into a stack frame that has returned
static fake_xhci g_fake;

// Builds the driver, its rings and its device in a zeroed fake
static int32_t attach_controller() {
    string::memset(&g_fake, 0, sizeof(g_fake));
    return attach_fake_xhci(g_fake) ? 0 : -1;
}

// A step still running keeps the fake, which is then never freed, so the step never wakes in freed memory
static int32_t detach_controller() {
    if (g_fake.step_task && !finish_step(g_fake)) {
        return -1;
    }

    detach_fake_xhci(g_fake);
    return 0;
}

BEFORE_EACH(usb_reconnect, attach_controller);
AFTER_EACH(usb_reconnect, detach_controller);

// Sets up the device on the root port, as a connect change on the port does
static void set_up_root_port(fake_xhci& fake) {
    xhci_hcd_test_access::setup_device(*fake.hcd, ROOT_PORT_INDEX);
}

// Setup stops at the reset, since memory keeps the 0 that acknowledging it writes to the write-1-to-clear enable bit
TEST(usb_reconnect, a_root_port_that_connected_again_tears_down_its_recorded_device_before_the_reset) {
    xhci_device* old_device = add_fake_device(g_fake, OLD_SLOT);
    ASSERT_NOT_NULL(old_device);
    xhci_hcd_test_access::record_port_device(*g_fake.hcd, ROOT_PORT_INDEX, old_device);

    drivers::xhci::xhci_portsc_register connected_again = {};
    connected_again.ccs = 1;
    connected_again.pp = 1;
    connected_again.csc = 1;
    *portsc(g_fake, ROOT_PORT_INDEX) = connected_again.raw;
    ASSERT_TRUE(start_step(g_fake, set_up_root_port));

    served_log served = {};
    ASSERT_TRUE(serve_until_step_done(g_fake, NEW_SLOT, served));
    ASSERT_TRUE(served.count >= 2);

    EXPECT_EQ(served.requests[0].kind, served_kind::command);
    EXPECT_EQ(served.requests[0].code, XHCI_TRB_TYPE_DISABLE_SLOT_CMD);
    EXPECT_EQ(served.requests[0].id, OLD_SLOT);

    EXPECT_EQ(served.requests[1].kind, served_kind::port_reset);
    EXPECT_EQ(served.requests[1].id, ROOT_PORT_INDEX);

    EXPECT_NULL(xhci_hcd_test_access::port_device(*g_fake.hcd, ROOT_PORT_INDEX));
    EXPECT_NULL(xhci_hcd_test_access::slot_device(*g_fake.hcd, OLD_SLOT));
}

// Handles a change on the root port, as the main loop does once a pass over the event ring records one
static void process_root_port_change(fake_xhci& fake) {
    xhci_hcd_test_access::process_port_change(*fake.hcd, ROOT_PORT_INDEX);
}

// The acknowledgment comes before the teardown's Disable Slot, so a device back during the teardown raises a connect
// change of its own
TEST(usb_reconnect, a_root_port_that_disconnected_is_acknowledged_before_its_device_is_torn_down) {
    xhci_device* old_device = add_fake_device(g_fake, OLD_SLOT);
    ASSERT_NOT_NULL(old_device);
    xhci_hcd_test_access::record_port_device(*g_fake.hcd, ROOT_PORT_INDEX, old_device);

    drivers::xhci::xhci_portsc_register disconnected = {};
    disconnected.pp = 1;
    disconnected.csc = 1;
    *portsc(g_fake, ROOT_PORT_INDEX) = disconnected.raw;
    ASSERT_TRUE(start_step(g_fake, process_root_port_change));

    served_log served = {};
    ASSERT_TRUE(serve_until_step_done(g_fake, NEW_SLOT, served));
    ASSERT_TRUE(served.count >= 2);

    EXPECT_EQ(served.requests[0].kind, served_kind::port_acknowledgment);
    EXPECT_EQ(served.requests[0].id, ROOT_PORT_INDEX);

    EXPECT_EQ(served.requests[1].kind, served_kind::command);
    EXPECT_EQ(served.requests[1].code, XHCI_TRB_TYPE_DISABLE_SLOT_CMD);
    EXPECT_EQ(served.requests[1].id, OLD_SLOT);

    EXPECT_NULL(xhci_hcd_test_access::port_device(*g_fake.hcd, ROOT_PORT_INDEX));
}

// Sets up a high-speed device on the hub port, as the hub driver's report of a connection does
static void set_up_hub_port(fake_xhci& fake) {
    xhci_hcd_test_access::setup_hub_device(*fake.hcd, fake.device, HUB_PORT, XHCI_USB_SPEED_HIGH_SPEED);
}

// The recorded device is torn down, and the new one is set up in a slot of its own and handed to the USB core
TEST(usb_reconnect, a_hub_port_that_connected_again_replaces_its_recorded_device) {
    xhci_device* old_device = add_fake_device(g_fake, OLD_SLOT);
    ASSERT_NOT_NULL(old_device);

    g_fake.device->set_is_hub(true);
    g_fake.device->set_hub_num_ports(HUB_PORT_COUNT);
    g_fake.device->set_hub_child(HUB_PORT, old_device);

    old_device->set_route_string(HUB_PORT);
    old_device->set_parent_slot_id(fake_xhci::SLOT_ID);
    old_device->set_parent_port_num(HUB_PORT);
    ASSERT_TRUE(start_step(g_fake, set_up_hub_port));

    served_log served = {};
    ASSERT_TRUE(serve_until_step_done(g_fake, NEW_SLOT, served));
    ASSERT_TRUE(served.count >= 2);

    EXPECT_EQ(served.requests[0].kind, served_kind::command);
    EXPECT_EQ(served.requests[0].code, XHCI_TRB_TYPE_DISABLE_SLOT_CMD);
    EXPECT_EQ(served.requests[0].id, OLD_SLOT);

    EXPECT_EQ(served.requests[1].kind, served_kind::command);
    EXPECT_EQ(served.requests[1].code, XHCI_TRB_TYPE_ENABLE_SLOT_CMD);
    EXPECT_EQ(served.requests[1].id, NEW_SLOT);

    xhci_device* new_device = g_fake.device->hub_child(HUB_PORT);
    ASSERT_NOT_NULL(new_device);
    EXPECT_EQ(new_device->slot_id(), NEW_SLOT);
    EXPECT_NOT_NULL(new_device->core_device());
    EXPECT_EQ(xhci_hcd_test_access::slot_device(*g_fake.hcd, NEW_SLOT), new_device);
    EXPECT_NULL(xhci_hcd_test_access::slot_device(*g_fake.hcd, OLD_SLOT));

    g_fake.device->set_hub_child(HUB_PORT, nullptr);
    xhci_hcd_test_access::forget_slot_device(*g_fake.hcd, new_device);
    usb::core::device_disconnected(g_fake.hcd, new_device);
    usb::core::finalize_disconnected_device(g_fake.hcd, new_device);
}

// The hub events the hub driver tests keep, more than the one each expects
static constexpr size_t MAX_HUB_EVENTS = 4;

// A failed read that leaves the hub's control endpoint running, so the driver recovers nothing before going on
static constexpr uint8_t FAILED_READ_CODE = XHCI_TRB_COMPLETION_CODE_DATA_BUFFER_ERROR;

static usb::hub::hub_driver* g_hub;

// Runs the hub driver's handling of the port's change on a task of its own, as its status change monitor's task does,
// then marks the step ended for `finish_step`
static void run_hub_port_change(void* arg) {
    auto* fake = static_cast<fake_xhci*>(arg);
    usb::hub::hub_driver_test_access::handle_port_change(*g_hub, HUB_PORT);
    fake->is_step_done.store_release(1);
    sched::exit(0);
}

// Clears the change a CLEAR_FEATURE names, and ends at once a reset that SET_FEATURE starts, with the port enabled at
// high speed
static void apply_hub_feature_request(hub_port_model& port, const xhci_device_request_packet& request) {
    bool is_change_cleared = request.bRequest == usb::hub::HUB_REQUEST_CLEAR_FEATURE &&
                             request.wValue >= usb::hub::PORT_FEATURE_C_CONNECTION;
    bool is_reset = request.bRequest == usb::hub::HUB_REQUEST_SET_FEATURE &&
                    request.wValue == usb::hub::PORT_FEATURE_RESET;

    if (is_change_cleared) {
        port.change &= static_cast<uint16_t>(~(1u << (request.wValue - usb::hub::PORT_FEATURE_C_CONNECTION)));
    } else if (is_reset) {
        port.status |= usb::hub::PORT_STATUS_ENABLE | usb::hub::PORT_STATUS_HIGH_SPEED;
        port.change |= usb::hub::PORT_CHANGE_RESET;
    }
}

// Answers the hub driver's request about the port from the model and has the HCD task handle the completion. The
// device can come back right after the first status read, before the driver clears the change that read reported
static void answer_hub_request(hub_port_model& port, const xhci_device_request_packet& request) {
    if (request.bRequest == usb::hub::HUB_REQUEST_GET_STATUS) {
        port.status_reads++;
        usb::hub::hub_port_status reply = {port.status, port.change};
        if (port.status_reads == 2 && port.is_second_read_failed) {
            complete_control_transfer(g_fake, g_fake.device, nullptr, 0, FAILED_READ_CODE);
        } else {
            complete_control_transfer(g_fake, g_fake.device, &reply, sizeof(reply));
        }

        if (port.status_reads == 1 && port.is_device_back_after_first_read) {
            port.status |= usb::hub::PORT_STATUS_CONNECTION;
        }
    } else {
        apply_hub_feature_request(port, request);
        complete_control_transfer(g_fake, g_fake.device, nullptr, 0);
    }

    xhci_hcd_test_access::process_events(*g_fake.hcd);
}

// Plays the hub and the HCD task's handling of hub events until the hub driver's step ends, giving the events it takes
// in `events`. False when the step outlives `test_helpers::SPIN_TIMEOUT_NS`
static bool serve_hub_until_step_done(hub_port_model& port, drivers::xhci::hub_event* events, size_t* event_count) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;

    while (!g_fake.is_step_done.load_acquire()) {
        if (clock::now_ns() > deadline) {
            return false;
        }

        xhci_device_request_packet request = {};
        bool is_rung = take_doorbell(g_fake, fake_xhci::SLOT_ID, XHCI_DOORBELL_TARGET_CONTROL_EP_RING);
        if (is_rung && read_control_request(g_fake.device, &request)) {
            answer_hub_request(port, request);
        }

        drivers::xhci::hub_event event = {};
        bool is_event_taken = xhci_hcd_test_access::take_hub_event(*g_fake.hcd, &event);
        if (is_event_taken && *event_count < MAX_HUB_EVENTS) {
            events[(*event_count)++] = event;
        }
    }

    return true;
}

// Runs the hub driver's handling of a change on the port, answered from `port`, giving the hub events it queued in
// `events`. False when the driver or its task cannot be made or the step does not end, and the driver then stays
static bool handle_hub_port_change(hub_port_model& port, drivers::xhci::hub_event* events, size_t* event_count) {
    g_hub = heap::ualloc_new<usb::hub::hub_driver>(&g_fake.usb_device, nullptr);
    if (!g_hub) {
        return false;
    }

    g_fake.step_task = test_helpers::start_pinned_task_on_another_cpu(run_hub_port_change, &g_fake, "usb_hub_task", 0);
    if (!g_fake.step_task || !serve_hub_until_step_done(port, events, event_count) || !finish_step(g_fake)) {
        return false;
    }

    heap::ufree_delete(g_hub);

    return true;
}

// The device leaves and comes back before the hub driver clears the connect change it read, so the hub raises no
// change for the return, and the driver must act on the port's status after the clear
TEST(usb_reconnect, a_hub_port_that_connected_again_before_its_change_was_cleared_is_set_up) {
    hub_port_model port = {0, usb::hub::PORT_CHANGE_CONNECTION, 0, true, false};
    drivers::xhci::hub_event events[MAX_HUB_EVENTS] = {};
    size_t event_count = 0;
    ASSERT_TRUE(handle_hub_port_change(port, events, &event_count));

    ASSERT_EQ(event_count, 1u);
    EXPECT_EQ(events[0].type, drivers::xhci::hub_event_type::enumerate);
    EXPECT_EQ(events[0].hub_port, HUB_PORT);
    EXPECT_EQ(events[0].speed, XHCI_USB_SPEED_HIGH_SPEED);
}

// The read after the clear fails, so the driver acts on the status read before it, whose change is already cleared
TEST(usb_reconnect, a_hub_port_whose_status_read_fails_after_the_clear_still_has_its_change_handled) {
    hub_port_model port = {0, usb::hub::PORT_CHANGE_CONNECTION, 0, false, true};
    drivers::xhci::hub_event events[MAX_HUB_EVENTS] = {};
    size_t event_count = 0;
    ASSERT_TRUE(handle_hub_port_change(port, events, &event_count));

    EXPECT_EQ(port.status_reads, 2u);
    ASSERT_EQ(event_count, 1u);
    EXPECT_EQ(events[0].type, drivers::xhci::hub_event_type::disconnect);
    EXPECT_EQ(events[0].hub_port, HUB_PORT);
}
