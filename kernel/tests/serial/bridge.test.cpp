#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/serial/ftdi.h"
#include "drivers/serial/serial.h"
#include "common/string.h"
#include "clock/clock.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
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

using namespace serial_test;
using usb_test::fake_xhci;

static constexpr uint8_t BULK_OUT = 0x02;
static constexpr uint16_t FT232BM_RELEASE = 0x0400;
static constexpr const char* SERIAL_NUMBER = "BR01";
static constexpr const char* PORT_PATH = "/dev/serial/ft232bm-br01";

// The first open's chip requests: the latency timer, the line's three, then DTR and RTS
static constexpr size_t FIRST_OPEN_REQUESTS = 5;

static constexpr size_t WRITE_BYTES = 5;

// Static, so calls that never end still find them after their test
static fake_chip                   g_chip;
static drivers::xhci::xhci_endpoint* g_bulk_out;
static usb::serial::bridge*        g_bridge;
static fs::file*                   g_file;
static int32_t                     g_rc;
static sync::atomic<uint32_t>      g_has_run_returned;
static const uint8_t               g_bytes[WRITE_BYTES] = {'h', 'e', 'l', 'l', 'o'};

static usb::device& chip_device() {
    return g_chip.controller->usb_device;
}

// An FT232BM on the fake controller, whose bulk OUT endpoint takes the bridge's sends. Its bulk IN endpoint is left
// off the controller, so the port's stream fails to open and the test plays no reads
static bool attach_bridge() {
    if (!attach_fake_chip(g_chip)) {
        return false;
    }

    fake_xhci& fake = *g_chip.controller;
    usb::device& dev = fake.usb_device;
    dev.bcd_device = FT232BM_RELEASE;
    string::memcpy(dev.serial_number, SERIAL_NUMBER, string::strlen(SERIAL_NUMBER) + 1);
    dev.num_interfaces = 1;

    usb::interface& iface = dev.interfaces[0];
    iface.interface_number = 0;
    iface.num_endpoints = 2;
    iface.endpoints[0] = {fake_xhci::BULK_IN, usb::USB_ENDPOINT_TYPE_BULK, fake_xhci::MAX_PACKET, 0};
    iface.endpoints[1] = {BULK_OUT, usb::USB_ENDPOINT_TYPE_BULK, fake_xhci::MAX_PACKET, 0};

    fake.device->set_endpoint(fake.endpoint->dci(), nullptr);
    g_bulk_out = usb_test::add_fake_endpoint(fake, BULK_OUT, fake_xhci::BULK_ATTRIBUTES, fake_xhci::MAX_PACKET);
    g_bridge = static_cast<usb::serial::bridge*>(usb::serial::create_bridge<usb::serial::ftdi>(&dev, &iface));
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

static void run_bridge(void*) {
    g_bridge->run();
    g_has_run_returned.store_release(1);
    sched::exit(0);
}

// True once the bridge's task has added the port to /dev/serial
static bool wait_until_listed() {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    fs::vattr attr = {};
    while (fs::stat(PORT_PATH, &attr) != fs::OK) {
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
    ASSERT_TRUE(attach_bridge());
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
    ASSERT_TRUE(attach_bridge());
    chip_request reset = {};
    ASSERT_TRUE(make_calls(g_chip, probe, &reset, 1));
    ASSERT_EQ(g_rc, 0);

    sched::task* runner = test_helpers::start_pinned_task_on_another_cpu(run_bridge, nullptr, "serial_bridge_run", 0);
    ASSERT_NOT_NULL(runner);
    ASSERT_TRUE(wait_until_listed());
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
