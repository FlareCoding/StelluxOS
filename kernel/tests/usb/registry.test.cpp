#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/usb/core/usb_core.h"
#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/core/usb_registry.h"
#include "drivers/usb/xhci/xhci.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "drivers/usb/xhci/xhci_device.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sync/atomic.h"
#include "sync/poll.h"

TEST_SUITE(usb_registry);

using test_helpers::spin_wait;

// The test driver's state that its test reads and sets, kept apart from the driver, which the core frees once the
// driver's task ends
struct listed_driver_record {
    sync::atomic<uint32_t> may_end;
    sync::atomic<uint32_t> is_freed;
};

static constexpr const char* LISTING_PATH = "/dev/sysinfo/usb";

// USB's application-specific class, which only the test driver matches
static constexpr uint8_t BOUND_CLASS = 0xFE;

// The vendor-specific class, which no driver matches for the test devices' IDs
static constexpr uint8_t UNBOUND_CLASS = 0xFF;

// IDs no driver lists, so only each interface's class decides its driver. They must stay unclaimed
static constexpr uint16_t TEST_VENDOR          = 0x0000;
static constexpr uint16_t DEVICE_PRODUCT       = 0x0001;
static constexpr uint16_t HUB_PRODUCT          = 0x0002;
static constexpr uint16_t HUB_CHILD_PRODUCT    = 0x0003;
static constexpr uint16_t NAMED_DEVICE_PRODUCT = 0x0004;

static constexpr uint8_t DEVICE_ROOT_PORT = 2;
static constexpr uint8_t HUB_ROOT_PORT    = 5;
static constexpr uint8_t HUB_FIRST_PORT   = 1;
static constexpr uint8_t HUB_THIRD_PORT   = 3;

static constexpr uint64_t END_POLL_MS = 1;

// Room for the lines of the test's devices and of any device another test leaves configured
static constexpr size_t LISTING_ROOM = 4096;

// A controller that drives no hardware, never freed, since a driver's task can release its device through it after
// its test ends
static drivers::xhci_hcd* s_controller;

// The record the next test driver the core creates reports to
static listed_driver_record* s_next_record;

// The listing file, opened by each test and closed by the AFTER_EACH, and its last snapshot
static fs::file* s_listing_file;
static char      s_listing[LISTING_ROOM];

// Runs until its test lets it end
class registry_test_driver : public usb::class_driver {
public:
    registry_test_driver(usb::device*, usb::interface*)
        : class_driver("registry_test"), m_record(s_next_record) {}

    ~registry_test_driver() override {
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

private:
    listed_driver_record* m_record;
};

REGISTER_USB_CLASS_DRIVER(registry_test_driver,
    USB_MATCH(BOUND_CLASS, usb::USB_MATCH_ANY, usb::USB_MATCH_ANY),
    USB_CLASS_DRIVER_FACTORY(registry_test_driver));

static int32_t usb_registry_before_all() {
    s_controller = heap::ualloc_new<drivers::xhci_hcd>(nullptr);
    return s_controller ? 0 : -1;
}

BEFORE_ALL(usb_registry, usb_registry_before_all);

static int32_t close_listing() {
    if (s_listing_file) {
        fs::close(s_listing_file);
        s_listing_file = nullptr;
    }

    return 0;
}

AFTER_EACH(usb_registry, close_listing);

// A device in slot `slot_id` on port `port` of `hub`, or of the root hub when `hub` is null, with an interface of the
// test driver's class and an interface no driver matches. Null when its memory cannot be allocated
static drivers::xhci::xhci_device* create_test_device(
    uint8_t slot_id,
    const drivers::xhci::xhci_device* hub,
    uint8_t port
) {
    auto* xdev = heap::ualloc_new<drivers::xhci::xhci_device>();
    if (!xdev) {
        return nullptr;
    }

    if (xdev->init(port, slot_id, XHCI_USB_SPEED_FULL_SPEED, false) != 0) {
        heap::ufree_delete(xdev);
        return nullptr;
    }

    if (xdev->set_port_path(hub, port) != 0) {
        xdev->destroy();
        heap::ufree_delete(xdev);
        return nullptr;
    }

    auto& bound = xdev->interface_info_mut(0);
    bound.interface_number = 0;
    bound.interface_class = BOUND_CLASS;
    bound.interface_subclass = 0x01;
    bound.interface_protocol = 0x02;

    auto& unbound = xdev->interface_info_mut(1);
    unbound.interface_number = 1;
    unbound.interface_class = UNBOUND_CLASS;

    xdev->set_num_interfaces(2);
    return xdev;
}

// Reports `xdev` configured as the test device `product`, whose test driver reports to `record`
static void configure(drivers::xhci::xhci_device* xdev, uint16_t product, listed_driver_record* record) {
    usb::usb_device_descriptor descriptor = {};
    descriptor.idVendor = TEST_VENDOR;
    descriptor.idProduct = product;

    s_next_record = record;
    usb::core::device_configured(s_controller, xdev, descriptor);
}

// Reports the device disconnected and then finalized, in the order a controller's teardown does
static void unplug(drivers::xhci::xhci_device* xdev) {
    usb::core::device_disconnected(s_controller, xdev);
    usb::core::finalize_disconnected_device(s_controller, xdev);
}

static void end_driver(listed_driver_record& record) {
    record.may_end.store_release(1);
    EXPECT_TRUE(spin_wait(record.is_freed));
}

static fs::file* open_listing() {
    s_listing_file = fs::open(LISTING_PATH, fs::O_RDONLY);
    return s_listing_file;
}

// Takes a new snapshot of the listing into s_listing, ended by a NUL
static void read_listing() {
    s_listing[0] = '\0';
    if (fs::seek(s_listing_file, 0, fs::SEEK_SET) != 0) {
        return;
    }

    size_t length = 0;
    ssize_t n = fs::read(s_listing_file, s_listing, sizeof(s_listing) - 1);
    while (n > 0) {
        length += static_cast<size_t>(n);
        n = fs::read(s_listing_file, s_listing + length, sizeof(s_listing) - 1 - length);
    }

    s_listing[length] = '\0';
}

static uint32_t poll_events() {
    uint32_t events = 0;
    RUN_ELEVATED(events = s_listing_file->get_node()->poll(s_listing_file, nullptr));
    return events;
}

// The listing's line for the device at `location`, or null when it lists none there
static const char* find_line(const char* location) {
    size_t location_length = string::strlen(location);
    const char* line = s_listing;
    while (*line) {
        if (string::strncmp(line, location, location_length) == 0 && line[location_length] == '\t') {
            return line;
        }

        while (*line && *line != '\n') {
            line++;
        }

        if (*line == '\n') {
            line++;
        }
    }

    return nullptr;
}

// The arrival that ends `line` right after `fields`, or 0 unless `line` is `fields`, digits and a line break
static uint64_t arrival_after(const char* line, const char* fields) {
    size_t fields_length = string::strlen(fields);
    if (!line || string::strncmp(line, fields, fields_length) != 0) {
        return 0;
    }

    uint64_t arrival = 0;
    const char* digit = line + fields_length;
    for (; *digit >= '0' && *digit <= '9'; digit++) {
        arrival = arrival * 10 + static_cast<uint64_t>(*digit - '0');
    }

    return *digit == '\n' ? arrival : 0;
}

// A test controller never attaches, so its number in each location is 0
static const char DEVICE_FIELDS[] = "0.2\t0000:0001\t00/00/00\t0:fe/01/02:registry_test,1:ff/00/00:-\t-\t-\t-\t";

static const char HUB_FIELDS[] = "0.5\t0000:0002\t00/00/00\t0:fe/01/02:registry_test,1:ff/00/00:-\t-\t-\t-\t";

static const char HUB_FIRST_CHILD_FIELDS[] =
    "0.5.1\t0000:0003\t00/00/00\t0:fe/01/02:registry_test,1:ff/00/00:-\t-\t-\t-\t";

static const char HUB_THIRD_CHILD_FIELDS[] =
    "0.5.3\t0000:0003\t00/00/00\t0:fe/01/02:registry_test,1:ff/00/00:-\t-\t-\t-\t";

static listed_driver_record s_order_records[4];

// The devices behind the hub on root port 5 arrive first, port 3's before port 1's, and the hub last. The listing goes
// by port, the hub right before the devices behind it, while each arrival counts one generation
TEST(usb_registry, each_configured_device_is_listed_in_port_order) {
    drivers::xhci::xhci_device* hub = create_test_device(1, nullptr, HUB_ROOT_PORT);
    drivers::xhci::xhci_device* third_child = create_test_device(2, hub, HUB_THIRD_PORT);
    drivers::xhci::xhci_device* first_child = create_test_device(3, hub, HUB_FIRST_PORT);
    drivers::xhci::xhci_device* device = create_test_device(4, nullptr, DEVICE_ROOT_PORT);

    ASSERT_NOT_NULL(hub);
    ASSERT_NOT_NULL(third_child);
    ASSERT_NOT_NULL(first_child);
    ASSERT_NOT_NULL(device);

    ASSERT_NOT_NULL(open_listing());
    configure(third_child, HUB_CHILD_PRODUCT, &s_order_records[0]);
    configure(first_child, HUB_CHILD_PRODUCT, &s_order_records[1]);
    configure(device, DEVICE_PRODUCT, &s_order_records[2]);
    configure(hub, HUB_PRODUCT, &s_order_records[3]);

    read_listing();
    const char* device_line = find_line("0.2");
    const char* hub_line = find_line("0.5");
    const char* first_child_line = find_line("0.5.1");
    const char* third_child_line = find_line("0.5.3");

    uint64_t first_arrival = arrival_after(third_child_line, HUB_THIRD_CHILD_FIELDS);
    EXPECT_NE(first_arrival, 0u);
    EXPECT_EQ(arrival_after(first_child_line, HUB_FIRST_CHILD_FIELDS), first_arrival + 1);
    EXPECT_EQ(arrival_after(device_line, DEVICE_FIELDS), first_arrival + 2);
    EXPECT_EQ(arrival_after(hub_line, HUB_FIELDS), first_arrival + 3);

    EXPECT_TRUE(device_line < hub_line);
    EXPECT_TRUE(hub_line < first_child_line);
    EXPECT_TRUE(first_child_line < third_child_line);

    unplug(device);
    unplug(first_child);
    unplug(third_child);
    unplug(hub);
    for (listed_driver_record& record : s_order_records) {
        end_driver(record);
    }
}

static listed_driver_record s_replug_records[2];

// A device unplugged and plugged back in between two reads shows an arrival two generations on, one per change
TEST(usb_registry, a_device_back_between_two_reads_shows_an_arrival_two_generations_on) {
    drivers::xhci::xhci_device* device = create_test_device(1, nullptr, DEVICE_ROOT_PORT);
    ASSERT_NOT_NULL(device);
    ASSERT_NOT_NULL(open_listing());

    configure(device, DEVICE_PRODUCT, &s_replug_records[0]);
    read_listing();
    uint64_t arrival = arrival_after(find_line("0.2"), DEVICE_FIELDS);
    EXPECT_NE(arrival, 0u);

    unplug(device);
    drivers::xhci::xhci_device* replugged = create_test_device(1, nullptr, DEVICE_ROOT_PORT);
    ASSERT_NOT_NULL(replugged);
    configure(replugged, DEVICE_PRODUCT, &s_replug_records[1]);
    read_listing();
    EXPECT_EQ(arrival_after(find_line("0.2"), DEVICE_FIELDS), arrival + 2);

    unplug(replugged);
    for (listed_driver_record& record : s_replug_records) {
        end_driver(record);
    }
}

static listed_driver_record s_ended_record;

static const char ENDED_DRIVER_FIELDS[] = "0.2\t0000:0001\t00/00/00\t0:fe/01/02:-,1:ff/00/00:-\t-\t-\t-\t";

TEST(usb_registry, a_device_whose_driver_ended_stays_listed_without_it) {
    drivers::xhci::xhci_device* device = create_test_device(1, nullptr, DEVICE_ROOT_PORT);
    ASSERT_NOT_NULL(device);
    ASSERT_NOT_NULL(open_listing());

    configure(device, DEVICE_PRODUCT, &s_ended_record);
    end_driver(s_ended_record);

    read_listing();
    EXPECT_NE(arrival_after(find_line("0.2"), ENDED_DRIVER_FIELDS), 0u);

    unplug(device);
}

static listed_driver_record s_watched_record;

// The device leaves the listing as its unplug begins, before its finalization
TEST(usb_registry, a_reader_is_told_when_a_device_arrives_or_leaves) {
    drivers::xhci::xhci_device* device = create_test_device(1, nullptr, DEVICE_ROOT_PORT);
    ASSERT_NOT_NULL(device);
    ASSERT_NOT_NULL(open_listing());
    EXPECT_EQ(poll_events(), sync::POLL_IN);
    read_listing();
    EXPECT_EQ(poll_events(), 0u);

    configure(device, DEVICE_PRODUCT, &s_watched_record);
    EXPECT_EQ(poll_events(), sync::POLL_IN);
    read_listing();
    EXPECT_EQ(poll_events(), 0u);
    EXPECT_NOT_NULL(find_line("0.2"));

    usb::core::device_disconnected(s_controller, device);
    EXPECT_EQ(poll_events(), sync::POLL_IN);
    read_listing();
    EXPECT_NULL(find_line("0.2"));

    usb::core::finalize_disconnected_device(s_controller, device);
    end_driver(s_watched_record);
}

// A device built as the core builds one, its strings written before it is listed, on root port 7 of controller 3
static usb::device s_named_device;

static const char NAMED_DEVICE_FIELDS[] =
    "3.7\t0000:0004\t02/00/00\t0:02/02/01:-,1:0a/00/00:-\tSN0042\tMaker Inc\tBoard Rev B\t";

static void build_named_device(usb::device& dev) {
    dev.controller_number = 3;
    dev.port_path[0] = 7;
    dev.port_path_length = 1;
    dev.lifetime_lock = sync::SPINLOCK_INIT;

    dev.vid = TEST_VENDOR;
    dev.pid = NAMED_DEVICE_PRODUCT;
    dev.device_class = 0x02;
    dev.num_interfaces = 2;

    usb::interface& control = dev.interfaces[0];
    control.interface_number = 0;
    control.interface_class = 0x02;
    control.interface_subclass = 0x02;
    control.interface_protocol = 0x01;

    usb::interface& data = dev.interfaces[1];
    data.interface_number = 1;
    data.interface_class = 0x0A;

    string::memcpy(dev.serial_number, "SN0042", sizeof("SN0042"));
    string::memcpy(dev.manufacturer, "Maker Inc", sizeof("Maker Inc"));
    string::memcpy(dev.product, "Board Rev B", sizeof("Board Rev B"));
}

TEST(usb_registry, a_device_is_listed_with_its_strings) {
    build_named_device(s_named_device);
    ASSERT_NOT_NULL(open_listing());
    usb::core::list_device(&s_named_device);

    read_listing();
    uint64_t arrival = arrival_after(find_line("3.7"), NAMED_DEVICE_FIELDS);

    usb::core::unlist_device(&s_named_device);
    EXPECT_NE(arrival, 0u);
}

// Devices added straight to the registry, as the core adds a configured device, on controllers 1 and 2
static usb::device s_first_controller_device;
static usb::device s_second_controller_device;

static void place_device(usb::device& dev, uint8_t controller_number, uint8_t root_port) {
    dev.controller_number = controller_number;
    dev.port_path[0] = root_port;
    dev.port_path_length = 1;
    dev.lifetime_lock = sync::SPINLOCK_INIT;
}

// The second controller's device on root port 1 arrives first, and the first controller's on root port 5 comes before
// it in the listing
TEST(usb_registry, devices_are_listed_by_controller_before_port) {
    place_device(s_second_controller_device, 2, 1);
    place_device(s_first_controller_device, 1, 5);
    ASSERT_NOT_NULL(open_listing());
    usb::core::list_device(&s_second_controller_device);
    usb::core::list_device(&s_first_controller_device);

    read_listing();
    const char* first_controller_line = find_line("1.5");
    const char* second_controller_line = find_line("2.1");

    usb::core::unlist_device(&s_first_controller_device);
    usb::core::unlist_device(&s_second_controller_device);
    ASSERT_NOT_NULL(first_controller_line);
    ASSERT_NOT_NULL(second_controller_line);
    EXPECT_TRUE(first_controller_line < second_controller_line);
}
