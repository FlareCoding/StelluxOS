#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/serial/cdc_acm.h"
#include "drivers/usb/serial/ch34x.h"
#include "drivers/usb/serial/cp210x.h"
#include "drivers/usb/serial/ftdi.h"
#include "drivers/serial/serial.h"
#include "terminal/tty.h"
#include "common/string.h"
#include "clock/clock.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sync/atomic.h"

namespace usb {

// Binds a class driver to its device as the USB core does when it creates one
struct class_driver_test_access {
    static void bind(class_driver& driver, device* dev, uint8_t interface_index) {
        driver.m_bound_device = dev;
        driver.m_bound_interface_index = interface_index;
    }
};

} // namespace usb

TEST_SUITE(serial_bridge);

struct reset_step {
    uint32_t lines;
    uint16_t set_mhs_value;
};

using namespace serial_test;
using test_helpers::user_page;
using test_helpers::user_space_scope;
using usb_test::fake_xhci;

using bridge_factory = usb::class_driver* (*)(usb::device* dev, usb::interface* iface);

static constexpr uint8_t BULK_OUT = 0x02;
static constexpr uint16_t FT232BM_RELEASE = 0x0400;
static constexpr const char* FTDI_SERIAL_NUMBER = "BR01";
static constexpr const char* PORT_PATH = "/dev/serial/ft232bm-br01";

// A CP2102 keeps the serial number 0001 unless its board's maker programs another
static constexpr uint16_t CP2102_RELEASE = 0x0100;
static constexpr uint8_t CP2102_PART_NUMBER = 0x02;
static constexpr const char* CP2102_SERIAL_NUMBER = "0001";
static constexpr const char* CP2102_PATH = "/dev/serial/cp2102-0001";

// The part number request, then IFC_ENABLE
static constexpr size_t CP210X_SETUP_REQUESTS = 2;

// A CP210x's first open sends SET_BAUDRATE, SET_LINE_CTL and SET_FLOW, then SET_MHS raising DTR and RTS
static constexpr size_t CP210X_FIRST_OPEN_REQUESTS = 4;
static constexpr uint8_t CP210X_SET_MHS = 0x07;
static constexpr uint8_t CP210X_GET_MDMSTS = 0x08;
static constexpr uint8_t CP210X_SET_FLOW = 0x13;
static constexpr uint16_t CP210X_FLOW_LENGTH = 16;

// esptool's reset on a Unix host, a TIOCMGET then one TIOCMSET for each step, and the SET_MHS each step sends
static constexpr reset_step TIGHT_RESET[] = {
    {0,                                         0x0300},
    {terminal::TIOCM_DTR | terminal::TIOCM_RTS, 0x0303},
    {terminal::TIOCM_RTS,                       0x0302},
    {terminal::TIOCM_DTR,                       0x0301},
    {0,                                         0x0300},
};

static constexpr size_t TIGHT_RESET_STEPS = sizeof(TIGHT_RESET) / sizeof(TIGHT_RESET[0]);

// A CDC-ACM device whose control interface 0 has a union descriptor naming data interface 1, which holds the bulk pair
static constexpr uint8_t ACM_SUBCLASS = 0x02;
static constexpr uint8_t DATA_CLASS = 0x0A;
static constexpr uint8_t ACM_DATA_INTERFACE = 1;
static constexpr uint16_t ACM_RELEASE = 0x0100;
static constexpr const char* ACM_SERIAL_NUMBER = "AC01";
static constexpr const char* ACM_PATH = "/dev/serial/acm-ac01";

// The control interface's notification endpoint, whose stream the bridge's task opens once the port is listed
static constexpr uint8_t ACM_NOTIFICATION_IN = 0x83;
static constexpr uint8_t INTERRUPT_ATTRIBUTES = 3;
static constexpr uint16_t ACM_NOTIFICATION_MAX_PACKET = 8;
static constexpr uint8_t ACM_NOTIFICATION_INTERVAL = 16;

static const uint8_t ACM_CONFIG[] = {
    9, usb::USB_DESCRIPTOR_CONFIGURATION, 53, 0, 2, 1, 0, 0x80, 50,                   // configuration, 53 bytes
    9, usb::USB_DESCRIPTOR_INTERFACE, 0, 0, 1, usb::USB_CLASS_CDC, ACM_SUBCLASS, 1, 0, // control interface 0
    5, 0x24, 0x06, 0, ACM_DATA_INTERFACE,                                              // union of 0 and 1
    7, usb::USB_DESCRIPTOR_ENDPOINT, ACM_NOTIFICATION_IN, 3, 8, 0, ACM_NOTIFICATION_INTERVAL,
    9, usb::USB_DESCRIPTOR_INTERFACE, ACM_DATA_INTERFACE, 0, 2, DATA_CLASS, 0, 0, 0,  // data interface 1
    7, usb::USB_DESCRIPTOR_ENDPOINT, BULK_OUT, 2, 64, 0, 0,
    7, usb::USB_DESCRIPTOR_ENDPOINT, fake_xhci::BULK_IN, 2, 64, 0, 0,
};

// The first open's class requests: SET_LINE_CODING, then SET_CONTROL_LINE_STATE raising DTR and RTS
static constexpr size_t ACM_FIRST_OPEN_REQUESTS = 2;
static constexpr uint8_t SET_LINE_CODING = 0x20;
static constexpr uint8_t SET_CONTROL_LINE_STATE = 0x22;
static constexpr uint16_t DTR_AND_RTS = 3;

// A CH340 names no serial number, so its port's name holds its location, here root port 5 of controller 1
static constexpr uint16_t CH340_PRODUCT_ID = 0x7523;
static constexpr uint16_t CH340_RELEASE = 0x0254;
static constexpr uint8_t CH340_CONTROLLER = 1;
static constexpr uint8_t CH340_ROOT_PORT = 5;
static constexpr const char* CH340_PATH = "/dev/serial/ch340-p1.5";

// The first open's chip requests: the latency timer, the line's three, then DTR and RTS
static constexpr size_t FIRST_OPEN_REQUESTS = 5;

static constexpr size_t WRITE_BYTES = 5;

// Static, so calls that never end still find them after their test
static fake_chip                   g_chip;
static drivers::xhci::xhci_endpoint* g_bulk_out;
static drivers::xhci::xhci_endpoint* g_notification_in;
static usb::serial::bridge*        g_bridge;
static fs::file*                   g_file;
static int32_t                     g_rc;
static user_page*                  g_page;
static sync::atomic<uint32_t>      g_has_run_returned;
static const uint8_t               g_bytes[WRITE_BYTES] = {'h', 'e', 'l', 'l', 'o'};

static usb::device& chip_device() {
    return g_chip.controller->usb_device;
}

// A chip whose bulk OUT endpoint takes the sends of a bridge from `create`. Its bulk IN endpoint is left off the fake
// controller, so the port's stream fails to open and the test plays no reads
static bool attach_bridge(bridge_factory create, uint16_t release, const char* serial_number, uint16_t product = 0) {
    if (!attach_fake_chip(g_chip)) {
        return false;
    }

    fake_xhci& fake = *g_chip.controller;
    usb::device& dev = fake.usb_device;
    dev.pid = product;
    dev.bcd_device = release;
    string::memcpy(dev.serial_number, serial_number, string::strlen(serial_number) + 1);
    dev.num_interfaces = 1;

    usb::interface& iface = dev.interfaces[0];
    iface.interface_number = 0;
    iface.num_endpoints = 2;
    iface.endpoints[0] = {fake_xhci::BULK_IN, usb::USB_ENDPOINT_TYPE_BULK, fake_xhci::MAX_PACKET, 0};
    iface.endpoints[1] = {BULK_OUT, usb::USB_ENDPOINT_TYPE_BULK, fake_xhci::MAX_PACKET, 0};

    fake.device->set_endpoint(fake.endpoint->dci(), nullptr);
    g_bulk_out = usb_test::add_fake_endpoint(fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    g_bridge = static_cast<usb::serial::bridge*>(create(&dev, &iface));
    if (!g_bulk_out || !g_bridge) {
        return false;
    }

    usb::class_driver_test_access::bind(*g_bridge, &dev, 0);
    g_has_run_returned.store_release(0);
    return true;
}

static void detach_bridge() {
    if (!g_chip.is_call_stuck) {
        heap::ufree_delete(g_bridge);
    }

    detach_fake_chip(g_chip);
}

static void probe() {
    usb::device& dev = chip_device();
    g_rc = g_bridge->probe(&dev, &dev.interfaces[0]);
}

static void open_port() {
    g_file = fs::open(PORT_PATH, fs::O_RDWR);
}

static void open_cp2102() {
    g_file = fs::open(CP2102_PATH, fs::O_RDWR);
}

// One step of esptool's reset, through the int at the start of the test's page
static int32_t set_lines_as_esptool_does(uint32_t lines) {
    user_space_scope scope(g_page->ctx);
    int32_t rc = fs::ioctl(g_file, terminal::TIOCMGET, g_page->addr);
    if (rc != fs::OK) {
        return rc;
    }

    *g_page->at<int32_t>(0) = static_cast<int32_t>(lines);
    return fs::ioctl(g_file, terminal::TIOCMSET, g_page->addr);
}

static void reset_as_esptool_does() {
    g_rc = fs::OK;
    for (const reset_step& step : TIGHT_RESET) {
        int32_t rc = fs::OK;
        RUN_ELEVATED(rc = set_lines_as_esptool_does(step.lines));
        g_rc = g_rc == fs::OK ? rc : g_rc;
    }
}

static void run_bridge(void*) {
    g_bridge->run();
    g_has_run_returned.store_release(1);
    sched::exit(0);
}

static bool attach_ft232bm() {
    return attach_bridge(usb::serial::create_bridge<usb::serial::ftdi>, FT232BM_RELEASE, FTDI_SERIAL_NUMBER);
}

// The bridge is bound to control interface 0, which lists its notification endpoint, and the bulk pair moves to data
// interface 1
static bool attach_acm() {
    if (!attach_bridge(usb::serial::create_bridge<usb::serial::cdc_acm>, ACM_RELEASE, ACM_SERIAL_NUMBER)) {
        return false;
    }

    usb::device& dev = chip_device();
    usb::interface& control = dev.interfaces[0];
    control.interface_class = usb::USB_CLASS_CDC;
    control.interface_subclass = ACM_SUBCLASS;
    control.num_endpoints = 1;
    control.endpoints[0] = {
        ACM_NOTIFICATION_IN,
        usb::USB_ENDPOINT_TYPE_INTERRUPT,
        ACM_NOTIFICATION_MAX_PACKET,
        ACM_NOTIFICATION_INTERVAL
    };

    usb::interface& data = dev.interfaces[1];
    data.interface_number = ACM_DATA_INTERFACE;
    data.interface_class = DATA_CLASS;
    data.num_endpoints = 2;
    data.endpoints[0] = {fake_xhci::BULK_IN, usb::USB_ENDPOINT_TYPE_BULK, fake_xhci::MAX_PACKET, 0};
    data.endpoints[1] = {BULK_OUT, usb::USB_ENDPOINT_TYPE_BULK, fake_xhci::MAX_PACKET, 0};

    dev.num_interfaces = 2;
    g_notification_in = usb_test::add_fake_endpoint(
        *g_chip.controller,
        ACM_NOTIFICATION_IN,
        INTERRUPT_ATTRIBUTES,
        ACM_NOTIFICATION_MAX_PACKET
    );
    return g_notification_in != nullptr;
}

static void open_acm_port() {
    g_file = fs::open(ACM_PATH, fs::O_RDWR);
}

// The notification read the bridge's task posts once the port is listed, or null when none comes before the deadline
static const drivers::xhci::xhci_normal_trb_t* wait_for_notification_read() {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    auto* read = usb_test::transfer_trb(*g_chip.controller, 0, g_notification_in);
    while (!read && clock::now_ns() <= deadline) {
        test_helpers::brief_delay();
        read = usb_test::transfer_trb(*g_chip.controller, 0, g_notification_in);
    }

    return reinterpret_cast<const drivers::xhci::xhci_normal_trb_t*>(read);
}

// The unplug ends the device's transfers, so a stream still open closes at run()'s end without a stop
static void unplug_chip() {
    drivers::xhci_hcd_test_access::mark_unplugged(*g_chip.controller->hcd, g_chip.controller->device);
    drivers::xhci_hcd_test_access::end_unplugged_transfers(*g_chip.controller->hcd, g_chip.controller->device);
}

// True once the bridge's task has added the port at `path` to /dev/serial
static bool wait_until_listed(const char* path) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    fs::vattr attr = {};
    while (fs::stat(path, &attr) != fs::OK) {
        if (clock::now_ns() > deadline) {
            return false;
        }

        test_helpers::brief_delay();
    }

    return true;
}

// Ends the send at entry `index` of the bulk OUT ring with all its bytes taken, as the controller does
static void complete_send(uint32_t index) {
    usb_test::post_transfer_event(*g_chip.controller, index, 0, XHCI_TRB_COMPLETION_CODE_SUCCESS, g_bulk_out);
    drivers::xhci_hcd_test_access::process_events(*g_chip.controller->hcd);
}

// A send before run() publishes the port has no port to report its end to, so the bridge refuses it
TEST(serial_bridge, a_send_before_the_port_is_published_is_refused) {
    ASSERT_TRUE(attach_ft232bm());
    chip_request reset = {};
    ASSERT_TRUE(make_calls(g_chip, probe, &reset, 1));
    ASSERT_EQ(g_rc, 0);

    EXPECT_EQ(g_bridge->send(g_bytes, WRITE_BYTES), serial::ERR_IO);
    EXPECT_NULL(usb_test::transfer_trb(*g_chip.controller, 0, g_bulk_out));

    detach_bridge();
}

// A send's completion reports its end, so the next send can start. After the unplug the bridge refuses new sends, and
// run() waits for the send in flight to call back before it unregisters the port and returns
TEST(serial_bridge, run_waits_for_the_send_in_flight_after_the_unplug) {
    ASSERT_TRUE(attach_ft232bm());
    chip_request reset = {};
    ASSERT_TRUE(make_calls(g_chip, probe, &reset, 1));
    ASSERT_EQ(g_rc, 0);

    sched::task* runner = test_helpers::start_pinned_task_on_another_cpu(run_bridge, nullptr, "serial_bridge_run", 0);
    ASSERT_NOT_NULL(runner);
    ASSERT_TRUE(wait_until_listed(PORT_PATH));
    chip_request first_open[FIRST_OPEN_REQUESTS];
    ASSERT_TRUE(make_calls(g_chip, open_port, first_open, FIRST_OPEN_REQUESTS));
    ASSERT_NOT_NULL(g_file);

    EXPECT_EQ(fs::write(g_file, g_bytes, WRITE_BYTES, fs::O_NONBLOCK), static_cast<ssize_t>(WRITE_BYTES));
    EXPECT_NOT_NULL(usb_test::transfer_trb(*g_chip.controller, 0, g_bulk_out));
    complete_send(0);
    EXPECT_EQ(fs::write(g_file, g_bytes, WRITE_BYTES, fs::O_NONBLOCK), static_cast<ssize_t>(WRITE_BYTES));
    EXPECT_NOT_NULL(usb_test::transfer_trb(*g_chip.controller, 1, g_bulk_out));

    g_bridge->disconnect();
    EXPECT_EQ(g_bridge->send(g_bytes, WRITE_BYTES), serial::ERR_IO);
    EXPECT_NULL(usb_test::transfer_trb(*g_chip.controller, 2, g_bulk_out));
    EXPECT_TRUE(test_helpers::blocks_before_deadline(runner));
    EXPECT_EQ(g_has_run_returned.load_acquire(), 0u);

    complete_send(1);
    EXPECT_TRUE(test_helpers::spin_wait(g_has_run_returned));
    test_helpers::unpin(runner);

    fs::close(g_file);
    detach_bridge();
}

// A CP2102 left with its factory serial number is named for the part its part number request reports, and its port
// carries esptool's reset as one SET_MHS for each step
TEST(serial_bridge, a_cp2102_with_the_factory_serial_number_is_cp2102_0001) {
    ASSERT_TRUE(attach_bridge(usb::serial::create_bridge<usb::serial::cp210x>, CP2102_RELEASE, CP2102_SERIAL_NUMBER));
    chip_request setup[CP210X_SETUP_REQUESTS];
    ASSERT_TRUE(make_calls(g_chip, probe, setup, CP210X_SETUP_REQUESTS, &CP2102_PART_NUMBER, 1));
    ASSERT_EQ(g_rc, 0);

    sched::task* runner = test_helpers::start_pinned_task_on_another_cpu(run_bridge, nullptr, "serial_bridge_run", 0);
    ASSERT_NOT_NULL(runner);
    ASSERT_TRUE(wait_until_listed(CP2102_PATH));

    // The first open raises DTR and RTS only after SET_FLOW has left both lowered, as they are on a new chip
    chip_request first_open[CP210X_FIRST_OPEN_REQUESTS];
    ASSERT_TRUE(make_calls(g_chip, open_cp2102, first_open, CP210X_FIRST_OPEN_REQUESTS));
    ASSERT_NOT_NULL(g_file);
    EXPECT_EQ(first_open[2].request, CP210X_SET_FLOW);
    EXPECT_EQ(first_open[3].request, CP210X_SET_MHS);
    EXPECT_EQ(first_open[3].value, static_cast<uint16_t>(0x0303));

    for (uint16_t offset = 0; offset < CP210X_FLOW_LENGTH; offset++) {
        EXPECT_EQ(first_open[2].data[offset], 0u);
    }

    user_page page;
    ASSERT_TRUE(page.ready());
    g_page = &page;

    chip_request reset[2 * TIGHT_RESET_STEPS];
    uint8_t status = 0;
    EXPECT_TRUE(make_calls(g_chip, reset_as_esptool_does, reset, 2 * TIGHT_RESET_STEPS, &status, 1));
    EXPECT_EQ(g_rc, fs::OK);

    for (size_t i = 0; i < TIGHT_RESET_STEPS; i++) {
        EXPECT_EQ(reset[2 * i].request, CP210X_GET_MDMSTS);
        EXPECT_EQ(reset[2 * i + 1].request, CP210X_SET_MHS);
        EXPECT_EQ(reset[2 * i + 1].value, TIGHT_RESET[i].set_mhs_value);
    }

    g_bridge->disconnect();
    EXPECT_TRUE(test_helpers::spin_wait(g_has_run_returned));
    test_helpers::unpin(runner);
    fs::close(g_file);
    detach_bridge();
}

// The port's requests go to the control interface, and the port's writes are sent on the bulk OUT endpoint of the
// data interface that the control interface's union descriptor names
TEST(serial_bridge, a_cdc_acm_port_sends_on_the_data_interface_the_union_descriptor_names) {
    ASSERT_TRUE(attach_acm());
    chip_request config_read = {};
    ASSERT_TRUE(make_calls(g_chip, probe, &config_read, 1, ACM_CONFIG, sizeof(ACM_CONFIG)));
    ASSERT_EQ(g_rc, 0);

    sched::task* runner = test_helpers::start_pinned_task_on_another_cpu(run_bridge, nullptr, "serial_bridge_run", 0);
    ASSERT_NOT_NULL(runner);
    ASSERT_TRUE(wait_until_listed(ACM_PATH));

    chip_request first_open[ACM_FIRST_OPEN_REQUESTS];
    ASSERT_TRUE(make_calls(g_chip, open_acm_port, first_open, ACM_FIRST_OPEN_REQUESTS));
    ASSERT_NOT_NULL(g_file);
    EXPECT_EQ(first_open[0].request, SET_LINE_CODING);
    EXPECT_EQ(first_open[1].request, SET_CONTROL_LINE_STATE);
    EXPECT_EQ(first_open[1].value, DTR_AND_RTS);

    EXPECT_EQ(fs::write(g_file, g_bytes, WRITE_BYTES, fs::O_NONBLOCK), static_cast<ssize_t>(WRITE_BYTES));
    EXPECT_NOT_NULL(usb_test::transfer_trb(*g_chip.controller, 0, g_bulk_out));
    complete_send(0);

    unplug_chip();
    g_bridge->disconnect();
    EXPECT_TRUE(test_helpers::spin_wait(g_has_run_returned));
    test_helpers::unpin(runner);

    fs::close(g_file);
    detach_bridge();
}

// The probe runs on the HCD task and posts no notification read, and the bridge's task opens the stream once the port
// is listed, with reads of one packet
TEST(serial_bridge, a_cdc_acm_ports_notifications_open_on_the_bridges_task) {
    ASSERT_TRUE(attach_acm());
    chip_request config_read = {};
    ASSERT_TRUE(make_calls(g_chip, probe, &config_read, 1, ACM_CONFIG, sizeof(ACM_CONFIG)));
    ASSERT_EQ(g_rc, 0);
    bool was_read_posted_by_probe = usb_test::transfer_trb(*g_chip.controller, 0, g_notification_in) != nullptr;

    sched::task* runner = test_helpers::start_pinned_task_on_another_cpu(run_bridge, nullptr, "serial_bridge_run", 0);
    ASSERT_NOT_NULL(runner);
    ASSERT_TRUE(wait_until_listed(ACM_PATH));
    const drivers::xhci::xhci_normal_trb_t* read = wait_for_notification_read();
    uint32_t read_length = read ? read->trb_transfer_length : 0;

    unplug_chip();
    g_bridge->disconnect();
    EXPECT_TRUE(test_helpers::spin_wait(g_has_run_returned));
    test_helpers::unpin(runner);
    detach_bridge();

    EXPECT_FALSE(was_read_posted_by_probe);
    EXPECT_EQ(read_length, static_cast<uint32_t>(ACM_NOTIFICATION_MAX_PACKET));
}

// The product ID names the chip, which reports no part number, and only the reset comes before the port is listed
TEST(serial_bridge, a_ch340_without_a_serial_number_is_named_for_its_location) {
    ASSERT_TRUE(attach_bridge(usb::serial::create_bridge<usb::serial::ch34x>, CH340_RELEASE, "", CH340_PRODUCT_ID));
    usb::device& dev = chip_device();
    dev.controller_number = CH340_CONTROLLER;
    dev.port_path[0] = CH340_ROOT_PORT;
    dev.port_path_length = 1;

    chip_request reset = {};
    ASSERT_TRUE(make_calls(g_chip, probe, &reset, 1));
    ASSERT_EQ(g_rc, 0);

    sched::task* runner = test_helpers::start_pinned_task_on_another_cpu(run_bridge, nullptr, "serial_bridge_run", 0);
    ASSERT_NOT_NULL(runner);
    EXPECT_TRUE(wait_until_listed(CH340_PATH));

    g_bridge->disconnect();
    EXPECT_TRUE(test_helpers::spin_wait(g_has_run_returned));
    test_helpers::unpin(runner);
    detach_bridge();
}
