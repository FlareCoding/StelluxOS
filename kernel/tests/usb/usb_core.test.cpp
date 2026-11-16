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
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::unpin;

// USB's diagnostic device class, which no other class driver matches
constexpr uint8_t TEST_INTERFACE_CLASS = 0xDC;

constexpr uint8_t TEST_ROOT_PORT = 1;

constexpr uint64_t END_POLL_MS = 1;

// A test driver's state that its test reads and sets, kept apart from the driver, which the core frees
struct test_driver_record {
    sync::atomic<uint32_t> is_created;
    sync::atomic<uint32_t> disconnects;
    sync::atomic<uint32_t> may_end;
    sync::atomic<uint32_t> is_freed;

    // Set by a test before configuring, so `disconnect()` or the destructor waits for the test there
    sync::atomic<uint32_t> holds_disconnect;
    sync::atomic<uint32_t> holds_destructor;

    sync::atomic<uint32_t> is_disconnecting;      // set once `disconnect()` waits for the test
    sync::atomic<uint32_t> may_leave_disconnect;
    sync::atomic<uint32_t> is_destroying;         // set once the destructor waits for the test
    sync::atomic<uint32_t> may_leave_destructor;

    // The device's handle on its HCD device as the destructor read it, which finalization clears before freeing it
    void* hcd_device_at_destruction;

    // How many drivers of the device's earlier interfaces had a started task when this driver's probe ran
    sync::atomic<uint32_t> started_tasks_at_probe;
};

// Two controllers that drive no hardware, so the tests choose every device's slot ID. They are never freed, since a
// driver's task can release its device through them after its test ends
static drivers::xhci_hcd* s_controller_a;
static drivers::xhci_hcd* s_controller_b;

// The records that the next test drivers created by the core report to, indexed by their interface's number
static test_driver_record* s_next_records;

// Sleeps until `flag` is set. Test drivers wait this way, since a spinning driver task on the test's CPU would starve
// the test, which runs on that CPU's idle task
static void sleep_until_set(const sync::atomic<uint32_t>& flag) {
    while (!flag.load_acquire()) {
        RUN_ELEVATED(sched::sleep_ms(END_POLL_MS));
    }
}

// Counts the core's `disconnect()` calls and runs until its test lets it end. A hold makes `disconnect()` or the
// destructor wait for the test. Each reads the driver's members first, in case the driver is freed during the wait
class usb_core_test_driver : public usb::class_driver {
public:
    usb_core_test_driver(usb::device*, usb::interface* iface)
        : class_driver("usb_core_test"), m_record(&s_next_records[iface->interface_number]) {
        m_record->is_created.store_release(1);
    }

    ~usb_core_test_driver() override {
        test_driver_record* record = m_record;
        usb::device* dev = bound_device();
        if (record->holds_destructor.load_acquire()) {
            record->is_destroying.store_release(1);
            sleep_until_set(record->may_leave_destructor);
        }

        record->hcd_device_at_destruction = dev ? dev->hcd_device : nullptr;
        record->is_freed.store_release(1);
    }

    int32_t probe(usb::device* dev, usb::interface* iface) override {
        auto iface_index = static_cast<uint8_t>(iface - dev->interfaces);
        uint32_t started_tasks = 0;
        for (uint8_t i = 0; i < iface_index; i++) {
            usb::class_driver* earlier = dev->bound_drivers[i];
            if (earlier && earlier->task()->state.load_acquire() != sched::TASK_STATE_CREATED) {
                started_tasks++;
            }
        }

        m_record->started_tasks_at_probe.store_release(started_tasks);

        return 0;
    }

    void run() override {
        sleep_until_set(m_record->may_end);
    }

    void disconnect() override {
        test_driver_record* record = m_record;
        record->disconnects.fetch_add_relaxed(1);
        if (record->holds_disconnect.load_acquire()) {
            record->is_disconnecting.store_release(1);
            sleep_until_set(record->may_leave_disconnect);
        }
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

// The task running interface `iface_index`'s driver of `xdev`, pinned so the test can watch it end, or null when the
// interface has no driver
static sched::task* pin_driver_task(drivers::xhci::xhci_device* xdev, uint8_t iface_index) {
    usb::device* dev = xdev->core_device();
    if (!dev || !dev->bound_drivers[iface_index]) {
        return nullptr;
    }

    sched::task* t = dev->bound_drivers[iface_index]->task();
    RUN_ELEVATED(t->add_ref());

    return t;
}

// True when `t` is seen dead, false when `SPIN_TIMEOUT_NS` passes first
static bool ends_before_deadline(sched::task* t) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (t->state.load_acquire() != sched::TASK_STATE_DEAD) {
        if (clock::now_ns() > deadline) {
            return false;
        }
    }

    return true;
}

static drivers::xhci::xhci_device* s_device_to_disconnect;
static sync::atomic<uint32_t> s_is_disconnect_done;

// Reports `s_device_to_disconnect` disconnected from controller A, as the HCD task's teardown does, then marks it done
static void run_disconnect(void*) {
    usb::core::device_disconnected(s_controller_a, s_device_to_disconnect);
    s_is_disconnect_done.store_release(1);
    sched::exit(0);
}

static test_driver_record s_disconnect_record;

// A driver's `run()` returns while the unplug's `disconnect()` call still runs, and the core frees the driver only
// once `disconnect()` returns, on the unplugging task
TEST(usb_core, a_driver_ending_during_its_disconnect_is_freed_after_disconnect_returns) {
    drivers::xhci::xhci_device* xdev = create_test_device(1, 1);
    ASSERT_NOT_NULL(xdev);
    s_disconnect_record.holds_disconnect.store_release(1);
    configure(s_controller_a, xdev, &s_disconnect_record);

    sched::task* driver_task = pin_driver_task(xdev, 0);
    ASSERT_NOT_NULL(driver_task);
    s_device_to_disconnect = xdev;
    sched::task* disconnect_task = start_pinned_task_on_another_cpu(run_disconnect, nullptr, "usb_disconnect", 0);
    ASSERT_NOT_NULL(disconnect_task);
    ASSERT_TRUE(spin_wait(s_disconnect_record.is_disconnecting));

    s_disconnect_record.may_end.store_release(1);
    EXPECT_TRUE(ends_before_deadline(driver_task));
    EXPECT_EQ(s_disconnect_record.is_freed.load_acquire(), 0u);

    s_disconnect_record.may_leave_disconnect.store_release(1);
    EXPECT_TRUE(spin_wait(s_is_disconnect_done));
    EXPECT_EQ(s_disconnect_record.is_freed.load_acquire(), 1u);

    unpin(driver_task);
    unpin(disconnect_task);
    usb::core::finalize_disconnected_device(s_controller_a, xdev);
}

static test_driver_record s_destructor_record;

// The device's teardown finishes while the destructor of its last driver still runs, and the core keeps the device
// until the destructor returns
TEST(usb_core, a_device_outlives_the_destructor_of_its_last_driver) {
    drivers::xhci::xhci_device* xdev = create_test_device(1, 1);
    ASSERT_NOT_NULL(xdev);
    s_destructor_record.holds_destructor.store_release(1);
    configure(s_controller_a, xdev, &s_destructor_record);

    usb::core::device_disconnected(s_controller_a, xdev);
    s_destructor_record.may_end.store_release(1);
    ASSERT_TRUE(spin_wait(s_destructor_record.is_destroying));
    usb::core::finalize_disconnected_device(s_controller_a, xdev);

    s_destructor_record.may_leave_destructor.store_release(1);
    EXPECT_TRUE(spin_wait(s_destructor_record.is_freed));
    EXPECT_EQ(s_destructor_record.hcd_device_at_destruction, static_cast<void*>(xdev));
}

static test_driver_record s_two_destructor_records[2];

// Both drivers end after the device's teardown, and the destructor of the one that ended last returns first. The core
// keeps the device until the other destructor returns too
TEST(usb_core, a_device_outlives_the_destructors_of_both_its_drivers) {
    test_driver_record& first_to_end = s_two_destructor_records[0];
    test_driver_record& last_to_end = s_two_destructor_records[1];
    drivers::xhci::xhci_device* xdev = create_test_device(1, 2);
    ASSERT_NOT_NULL(xdev);
    first_to_end.holds_destructor.store_release(1);
    last_to_end.holds_destructor.store_release(1);

    configure(s_controller_a, xdev, s_two_destructor_records);
    sched::task* last_task = pin_driver_task(xdev, 1);
    ASSERT_NOT_NULL(last_task);
    unplug(s_controller_a, xdev);

    first_to_end.may_end.store_release(1);
    ASSERT_TRUE(spin_wait(first_to_end.is_destroying));
    last_to_end.may_end.store_release(1);
    ASSERT_TRUE(spin_wait(last_to_end.is_destroying));

    last_to_end.may_leave_destructor.store_release(1);
    EXPECT_TRUE(ends_before_deadline(last_task));
    first_to_end.may_leave_destructor.store_release(1);
    EXPECT_TRUE(spin_wait(first_to_end.is_freed));

    unpin(last_task);
    EXPECT_EQ(last_to_end.hcd_device_at_destruction, static_cast<void*>(xdev));
    EXPECT_EQ(first_to_end.hcd_device_at_destruction, static_cast<void*>(xdev));
}

static test_driver_record s_probe_order_records[3];

// The core starts a device's driver tasks only once its last interface is probed, so no interface is probed while the
// driver of an earlier one runs
TEST(usb_core, a_devices_driver_tasks_start_after_all_its_interfaces_are_probed) {
    drivers::xhci::xhci_device* xdev = create_test_device(1, 3);
    ASSERT_NOT_NULL(xdev);

    configure(s_controller_a, xdev, s_probe_order_records);
    EXPECT_EQ(s_probe_order_records[1].started_tasks_at_probe.load_acquire(), 0u);
    EXPECT_EQ(s_probe_order_records[2].started_tasks_at_probe.load_acquire(), 0u);

    unplug(s_controller_a, xdev);
    for (test_driver_record& record : s_probe_order_records) {
        end_driver(record);
    }
}
