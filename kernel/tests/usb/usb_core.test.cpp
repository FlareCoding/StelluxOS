#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/usb/core/usb_core.h"
#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/xhci/xhci.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "drivers/usb/xhci/xhci_device.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sync/atomic.h"

TEST_SUITE(usb_core);

using test_helpers::spin_wait;

// USB's diagnostic device class, which no other class driver matches
constexpr uint8_t TEST_INTERFACE_CLASS = 0xDC;

constexpr uint8_t TEST_ROOT_PORT = 1;

constexpr uint64_t END_POLL_MS = 1;

// A test driver's state that its test reads and sets, kept apart from the driver, which the core frees once the
// driver's task ends
struct test_driver_record {
    sync::atomic<uint32_t> is_created;
    sync::atomic<uint32_t> disconnects;
    sync::atomic<uint32_t> may_end;
    sync::atomic<uint32_t> is_freed;
};

// Two controllers that drive no hardware, so the tests choose every device's slot ID. They are never freed, since a
// driver's task can release its device through them after its test ends
static drivers::xhci_hcd* s_controller_a;
static drivers::xhci_hcd* s_controller_b;

// The records that the next test drivers created by the core report to, indexed by their interface's number
static test_driver_record* s_next_records;

// Counts the core's `disconnect()` calls and runs until its test lets it end
class usb_core_test_driver : public usb::class_driver {
public:
    usb_core_test_driver(usb::device*, usb::interface* iface)
        : class_driver("usb_core_test"), m_record(&s_next_records[iface->interface_number]) {
        m_record->is_created.store_release(1);
    }

    ~usb_core_test_driver() override {
        m_record->is_freed.store_release(1);
    }

    int32_t probe(usb::device*, usb::interface*) override {
        return 0;
    }

    void run() override {
        while (!m_record->may_end.load_acquire()) {
            RUN_ELEVATED(sched::sleep_ms(END_POLL_MS));
        }
    }

    void disconnect() override {
        m_record->disconnects.fetch_add_relaxed(1);
    }

private:
    test_driver_record* m_record;
};

REGISTER_USB_CLASS_DRIVER(usb_core_test_driver,
    USB_MATCH(TEST_INTERFACE_CLASS, usb::USB_MATCH_ANY, usb::USB_MATCH_ANY),
    USB_CLASS_DRIVER_FACTORY(usb_core_test_driver));

static int32_t usb_core_before_all() {
    s_controller_a = heap::ualloc_new<drivers::xhci_hcd>(nullptr);
    s_controller_b = heap::ualloc_new<drivers::xhci_hcd>(nullptr);
    return s_controller_a && s_controller_b ? 0 : -1;
}

BEFORE_ALL(usb_core, usb_core_before_all);

// A device in slot `slot_id` with `interface_count` interfaces of the test class, which only the test driver matches,
// or null when its memory cannot be allocated. Its port, speed and context size matter only to a controller
static drivers::xhci::xhci_device* create_test_device(uint8_t slot_id, uint8_t interface_count) {
    auto* xdev = heap::ualloc_new<drivers::xhci::xhci_device>();
    if (!xdev) {
        return nullptr;
    }

    if (xdev->init(TEST_ROOT_PORT, slot_id, XHCI_USB_SPEED_HIGH_SPEED, false) != 0) {
        heap::ufree_delete(xdev);
        return nullptr;
    }

    for (uint8_t i = 0; i < interface_count; i++) {
        xdev->interface_info_mut(i).interface_number = i;
        xdev->interface_info_mut(i).interface_class = TEST_INTERFACE_CLASS;
    }

    xdev->set_num_interfaces(interface_count);
    return xdev;
}

// Reports the device configured on behalf of `controller`, so the test driver bound to interface N reports to
// `records[N]`
static void configure(
    drivers::xhci_hcd* controller,
    drivers::xhci::xhci_device* xdev,
    test_driver_record* records
) {
    usb::usb_device_descriptor descriptor = {};
    s_next_records = records;
    usb::core::device_configured(controller, xdev, descriptor);
}

// Reports the device disconnected and then finalized, in the order a controller's teardown does. The core frees the
// device at once, or on the task of the last of its drivers to end
static void unplug(drivers::xhci_hcd* controller, drivers::xhci::xhci_device* xdev) {
    usb::core::device_disconnected(controller, xdev);
    usb::core::finalize_disconnected_device(controller, xdev);
}

// Lets the test driver end, if the core created one, and waits until the core frees it
static void end_driver(test_driver_record& record) {
    record.may_end.store_release(1);
    if (record.is_created.load_acquire()) {
        EXPECT_TRUE(spin_wait(record.is_freed));
    }
}

// Slots 1 and 2 on each of the two controllers
constexpr size_t SAME_SLOT_DEVICES = 4;

static test_driver_record s_same_slot_records[SAME_SLOT_DEVICES];

// With devices in slots 1 and 2 of both controllers, each unplug reaches only the unplugged device's driver
TEST(usb_core, each_unplug_reaches_only_its_devices_driver_when_two_controllers_use_the_same_slot_ids) {
    drivers::xhci_hcd* controllers[SAME_SLOT_DEVICES] = {
        s_controller_a,
        s_controller_a,
        s_controller_b,
        s_controller_b,
    };

    uint8_t slot_ids[SAME_SLOT_DEVICES] = {1, 2, 1, 2};
    drivers::xhci::xhci_device* devices[SAME_SLOT_DEVICES] = {};
    for (size_t i = 0; i < SAME_SLOT_DEVICES; i++) {
        devices[i] = create_test_device(slot_ids[i], 1);
        ASSERT_NOT_NULL(devices[i]);
    }

    for (size_t i = 0; i < SAME_SLOT_DEVICES; i++) {
        configure(controllers[i], devices[i], &s_same_slot_records[i]);
        EXPECT_EQ(s_same_slot_records[i].is_created.load_acquire(), 1u);
    }

    for (size_t i = 0; i < SAME_SLOT_DEVICES; i++) {
        unplug(controllers[i], devices[i]);
        for (size_t j = 0; j < SAME_SLOT_DEVICES; j++) {
            uint32_t expected_disconnects = j <= i ? 1 : 0;
            EXPECT_EQ(s_same_slot_records[j].disconnects.load_acquire(), expected_disconnects);
        }
    }

    for (test_driver_record& record : s_same_slot_records) {
        end_driver(record);
    }
}

static test_driver_record s_earlier_record;
static test_driver_record s_later_record;

// A slot is reused while the driver of its earlier device still runs, and the later device's unplug reaches the later
// device's driver
TEST(usb_core, a_slot_reused_while_its_earlier_driver_runs_keeps_the_later_devices_driver) {
    drivers::xhci::xhci_device* earlier_device = create_test_device(1, 1);
    drivers::xhci::xhci_device* later_device = create_test_device(1, 1);
    ASSERT_NOT_NULL(earlier_device);
    ASSERT_NOT_NULL(later_device);

    configure(s_controller_a, earlier_device, &s_earlier_record);
    unplug(s_controller_a, earlier_device);
    configure(s_controller_a, later_device, &s_later_record);
    end_driver(s_earlier_record);

    unplug(s_controller_a, later_device);
    EXPECT_EQ(s_earlier_record.disconnects.load_acquire(), 1u);
    EXPECT_EQ(s_later_record.disconnects.load_acquire(), 1u);
    end_driver(s_later_record);
}

static test_driver_record s_two_interface_records[2];

// The driver of interface 1 ends before the device's unplug, so the unplug reaches only interface 0's driver
TEST(usb_core, a_driver_that_ended_before_its_devices_unplug_gets_no_disconnect) {
    test_driver_record& running = s_two_interface_records[0];
    test_driver_record& ended = s_two_interface_records[1];
    drivers::xhci::xhci_device* xdev = create_test_device(1, 2);
    ASSERT_NOT_NULL(xdev);

    configure(s_controller_a, xdev, s_two_interface_records);
    usb::device* dev = xdev->core_device();
    ASSERT_NOT_NULL(dev);
    ASSERT_NOT_NULL(dev->bound_drivers[1]);

    end_driver(ended);
    ASSERT_NULL(dev->bound_drivers[1]);
    ASSERT_NOT_NULL(dev->bound_drivers[0]);

    unplug(s_controller_a, xdev);
    EXPECT_EQ(running.disconnects.load_acquire(), 1u);
    EXPECT_EQ(ended.disconnects.load_acquire(), 0u);
    end_driver(running);
}
