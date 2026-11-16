#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "drivers/serial/serial.h"
#include "drivers/serial/port.h"
#include "fs/devfs/devfs.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sync/poll.h"
#include "tests/serial/fake_port_driver.h"

TEST_SUITE(serial_listing);

// The driver of every port the tests register, whose chip answers at once
static serial_test::fake_port_driver g_driver;

static constexpr const char* LISTING_PATH = "/dev/sysinfo/serial";

// Room for the listing of a few ports
static constexpr size_t LISTING_BYTES = 4096;

// Room for one field's text and its NUL
static constexpr size_t FIELD_BYTES = 64;

// The listing's fields, counted from 0
static constexpr size_t MANUFACTURER_FIELD = 6;
static constexpr size_t PRODUCT_FIELD = 7;
static constexpr size_t ARRIVAL_FIELD = 8;
static constexpr size_t RECEIVED_FIELD = 9;
static constexpr size_t DROPPED_FIELD = 10;
static constexpr size_t CHANNEL_FIELD = 11;

// A board no other test names, with every field the listing shows
static constexpr serial::identity BOARD = {
    .chip = "listchip",
    .serial_number = "LS0042",
    .location = "9.5",
    .channel = -1,
    .bus = "usb",
    .driver = "listdrv",
    .vid = 0x10c4,
    .pid = 0xea60,
    .manufacturer = "Maker Inc",
    .product = "Board Rev B",
};

static constexpr const char* BOARD_NAME = "listchip-0042";

static char g_listing[LISTING_BYTES];

// The listing file and the ports a test has not unplugged yet, which the AFTER_EACH cleans up when an ASSERT ends it
struct test_port {
    serial::port* port;
    bool          is_detached;
};

static constexpr size_t MAX_TEST_PORTS = 4;
static test_port g_test_ports[MAX_TEST_PORTS];
static fs::file* g_listing_file;

static test_port* find_test_port(serial::port* p) {
    for (test_port& tracked : g_test_ports) {
        if (tracked.port == p) {
            return &tracked;
        }
    }

    return nullptr;
}

static fs::file* open_listing() {
    g_listing_file = fs::open(LISTING_PATH, fs::O_RDONLY);
    return g_listing_file;
}

static serial::port* plug(const serial::identity& id) {
    serial::port* p = serial::register_port(&g_driver, id);
    test_port* free_slot = find_test_port(nullptr);
    if (p && free_slot) {
        *free_slot = {.port = p, .is_detached = false};
    }

    return p;
}

static void detach_port(serial::port* p) {
    serial::detach(p);
    test_port* tracked = find_test_port(p);
    if (tracked) {
        tracked->is_detached = true;
    }
}

static void unregister_detached_port(serial::port* p) {
    serial::unregister_port(p);
    test_port* tracked = find_test_port(p);
    if (tracked) {
        *tracked = {};
    }
}

// What a driver does when its device leaves
static void unplug(serial::port* p) {
    detach_port(p);
    unregister_detached_port(p);
}

// A port a test detached but did not unregister stays registered, since a test stops there only when the port stayed
// listed, and the listing's next read would walk a port freed while listed
static int32_t clean_up_after_test() {
    for (test_port& tracked : g_test_ports) {
        if (tracked.port && !tracked.is_detached) {
            unplug(tracked.port);
        }

        tracked = {};
    }

    if (g_listing_file) {
        fs::close(g_listing_file);
        g_listing_file = nullptr;
    }

    return 0;
}

AFTER_EACH(serial_listing, clean_up_after_test);

// Takes a new snapshot of the listing into g_listing, ended by a NUL
static void read_listing(fs::file* f) {
    g_listing[0] = '\0';
    if (fs::seek(f, 0, fs::SEEK_SET) != 0) {
        return;
    }

    size_t length = 0;
    ssize_t n = fs::read(f, g_listing, sizeof(g_listing) - 1);
    while (n > 0) {
        length += static_cast<size_t>(n);
        n = fs::read(f, g_listing + length, sizeof(g_listing) - 1 - length);
    }

    g_listing[length] = '\0';
}

// The line of g_listing whose first field is `name`, or null
static const char* find_line(const char* name) {
    size_t name_length = string::strlen(name);
    for (const char* line = g_listing; *line != '\0';) {
        if (string::strncmp(line, name, name_length) == 0 && line[name_length] == '\t') {
            return line;
        }

        while (*line != '\0' && *line != '\n') {
            line++;
        }

        if (*line == '\n') {
            line++;
        }
    }

    return nullptr;
}

// Copies field `index` of `line` into `out`, false when the line has fewer fields
static bool copy_field(const char* line, size_t index, char (&out)[FIELD_BYTES]) {
    for (size_t i = 0; i < index; i++) {
        while (*line != '\t') {
            if (*line == '\0' || *line == '\n') {
                return false;
            }

            line++;
        }

        line++;
    }

    size_t length = 0;
    while (line[length] != '\t' && line[length] != '\n' && line[length] != '\0' && length + 1 < FIELD_BYTES) {
        out[length] = line[length];
        length++;
    }

    out[length] = '\0';
    return true;
}

// Field `index` of `line` as a number, 0 when the field is missing or starts with no digit
static uint64_t field_number(const char* line, size_t index) {
    char text[FIELD_BYTES] = {};
    uint64_t value = 0;
    if (copy_field(line, index, text)) {
        for (const char* c = text; *c >= '0' && *c <= '9'; c++) {
            value = value * 10 + static_cast<uint64_t>(*c - '0');
        }
    }

    return value;
}

// True when `line` reads `before_arrival`, the arrival it lists, then `after_arrival`, since other tests' ports move
// the generation
static bool is_line(const char* line, const char* before_arrival, const char* after_arrival) {
    char expected[FIELD_BYTES * 4] = {};
    size_t length = devfs::append_str(expected, sizeof(expected), 0, before_arrival);
    length = devfs::append_u64(expected, sizeof(expected), length, field_number(line, ARRIVAL_FIELD));
    length = devfs::append_str(expected, sizeof(expected), length, after_arrival);
    return length < sizeof(expected) && string::strncmp(line, expected, length) == 0;
}

static uint32_t poll_events(fs::file* f) {
    uint32_t events = 0;
    RUN_ELEVATED(events = f->get_node()->poll(f, nullptr));
    return events;
}

TEST(serial_listing, a_port_is_listed_with_every_field_until_it_is_detached) {
    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);

    read_listing(f);
    const char* line = find_line(BOARD_NAME);
    ASSERT_NOT_NULL(line);
    EXPECT_TRUE(is_line(
        line, "listchip-0042\tusb\tlistdrv\t10c4:ea60\t9.5\tLS0042\tMaker Inc\tBoard Rev B\t", "\t0\t0\t-\n"
    ));

    detach_port(p);
    read_listing(f);
    ASSERT_NULL(find_line(BOARD_NAME));

    unregister_detached_port(p);
}

TEST(serial_listing, a_port_keeps_its_own_copy_of_each_listed_string) {
    char bus[] = "usb";
    char driver[] = "listdrv";
    char manufacturer[] = "Maker Inc";
    char product[] = "Board Rev B";

    serial::identity board = BOARD;
    board.bus = bus;
    board.driver = driver;
    board.manufacturer = manufacturer;
    board.product = product;

    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    serial::port* p = plug(board);
    ASSERT_NOT_NULL(p);

    // A driver's strings need not outlive the registration
    bus[0] = 'X';
    driver[0] = 'X';
    manufacturer[0] = 'X';
    product[0] = 'X';

    read_listing(f);
    const char* line = find_line(BOARD_NAME);
    ASSERT_NOT_NULL(line);
    EXPECT_TRUE(is_line(
        line, "listchip-0042\tusb\tlistdrv\t10c4:ea60\t9.5\tLS0042\tMaker Inc\tBoard Rev B\t", "\t0\t0\t-\n"
    ));

    unplug(p);
}

TEST(serial_listing, missing_values_are_listed_as_dashes) {
    serial::identity bare = {.chip = "listchip", .serial_number = "", .location = "9.6", .channel = -1};
    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    serial::port* p = plug(bare);
    ASSERT_NOT_NULL(p);

    read_listing(f);
    const char* line = find_line("listchip-p9.6");
    ASSERT_NOT_NULL(line);
    EXPECT_TRUE(is_line(line, "listchip-p9.6\t-\t-\t-\t9.6\t-\t-\t-\t", "\t0\t0\t-\n"));

    unplug(p);
}

TEST(serial_listing, control_characters_in_a_devices_strings_become_spaces) {
    serial::identity board = BOARD;
    board.manufacturer = "Tab\there";
    board.product = "Line\nend\x7f\x1b[2J\x80";

    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    serial::port* p = plug(board);
    ASSERT_NOT_NULL(p);

    read_listing(f);
    const char* line = find_line(BOARD_NAME);
    ASSERT_NOT_NULL(line);

    char text[FIELD_BYTES] = {};
    ASSERT_TRUE(copy_field(line, MANUFACTURER_FIELD, text));
    EXPECT_STREQ(text, "Tab here");

    // A byte from 0x80 up is kept. The product is compared without printing it, since a failure message holding that
    // byte raw breaks the runner's text tools
    const char* product_start = "Line end  [2J";
    size_t kept_at = string::strlen(product_start);
    ASSERT_TRUE(copy_field(line, PRODUCT_FIELD, text));
    EXPECT_EQ(string::strncmp(text, product_start, kept_at), 0);
    EXPECT_EQ(static_cast<uint8_t>(text[kept_at]), 0x80u);
    EXPECT_EQ(text[kept_at + 1], '\0');

    unplug(p);
}

TEST(serial_listing, every_byte_counts_as_received_and_those_a_full_queue_cannot_hold_as_dropped) {
    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);

    size_t sent_bytes = serial::RECEIVE_QUEUE_BYTES + 10;
    auto* bytes = static_cast<uint8_t*>(heap::uzalloc(sent_bytes));
    ASSERT_NOT_NULL(bytes);

    // Dropped while no file has the port open, which the dropped count leaves out
    serial::receive(p, bytes, 5);
    fs::file* port_file = fs::open("/dev/serial/listchip-0042", fs::O_RDONLY);
    ASSERT_NOT_NULL(port_file);
    serial::receive(p, bytes, sent_bytes);

    read_listing(f);
    const char* line = find_line(BOARD_NAME);
    ASSERT_NOT_NULL(line);
    EXPECT_EQ(field_number(line, RECEIVED_FIELD), 5 + sent_bytes);
    EXPECT_EQ(field_number(line, DROPPED_FIELD), 10u);

    heap::ufree(bytes);
    fs::close(port_file);
    unplug(p);
}

TEST(serial_listing, a_channel_is_listed_by_the_letter_its_name_ends_in) {
    serial::identity channel_b = BOARD;
    channel_b.channel = 1;
    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    serial::port* p = plug(channel_b);
    ASSERT_NOT_NULL(p);

    read_listing(f);
    const char* line = find_line("listchip-0042.b");
    ASSERT_NOT_NULL(line);

    char text[FIELD_BYTES] = {};
    ASSERT_TRUE(copy_field(line, CHANNEL_FIELD, text));
    EXPECT_STREQ(text, "b");

    unplug(p);
}

TEST(serial_listing, each_arrival_and_departure_counts_once) {
    serial::identity twin = BOARD;
    twin.location = "9.8";
    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);

    serial::port* first = plug(BOARD);
    serial::port* second = plug(twin);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    read_listing(f);
    const char* first_line = find_line(BOARD_NAME);
    const char* second_line = find_line("listchip-ls0042");
    ASSERT_NOT_NULL(first_line);
    ASSERT_NOT_NULL(second_line);

    uint64_t first_arrival = field_number(first_line, ARRIVAL_FIELD);
    EXPECT_EQ(field_number(second_line, ARRIVAL_FIELD), first_arrival + 1);

    // The first board leaves and comes back
    unplug(first);
    serial::port* back = plug(BOARD);
    ASSERT_NOT_NULL(back);

    read_listing(f);
    const char* back_line = find_line(BOARD_NAME);
    ASSERT_NOT_NULL(back_line);
    EXPECT_EQ(field_number(back_line, ARRIVAL_FIELD), first_arrival + 3);

    unplug(back);
    unplug(second);
}

TEST(serial_listing, a_reader_is_told_when_a_port_arrives_or_leaves) {
    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(poll_events(f), sync::POLL_IN);
    read_listing(f);
    EXPECT_EQ(poll_events(f), 0u);

    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    EXPECT_EQ(poll_events(f), sync::POLL_IN);
    read_listing(f);
    EXPECT_EQ(poll_events(f), 0u);

    detach_port(p);
    EXPECT_EQ(poll_events(f), sync::POLL_IN);
    read_listing(f);
    ASSERT_NULL(find_line(BOARD_NAME));

    unregister_detached_port(p);
}

// What the listing's wake found of a departing port, which `record_departure` writes under the registry's lock
static serial::port* g_departing_port;
static uint32_t g_departure_wakes;
static bool g_wake_found_the_file;
static bool g_wake_found_the_name;

static rc::strong_ref<sched::task> record_departure(sync::wait_observer&) {
    g_departure_wakes++;
    g_wake_found_the_file = g_departing_port->node()->parent() != nullptr;
    g_wake_found_the_name = g_departing_port->name_record().link.is_linked();
    return {};
}

// A watcher woken by a departure must not find the name free while the listing still shows the port
TEST(serial_listing, a_port_leaves_the_listing_before_its_file_and_name) {
    fs::file* f = open_listing();
    ASSERT_NOT_NULL(f);
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);

    g_departing_port = p;
    g_departure_wakes = 0;
    sync::poll_table pt;
    pt.init(sched::current());
    pt.notify = record_departure;
    RUN_ELEVATED((void)f->get_node()->poll(f, &pt));

    detach_port(p);
    RUN_ELEVATED(sync::poll_cleanup(pt));
    EXPECT_EQ(g_departure_wakes, 1u);
    EXPECT_TRUE(g_wake_found_the_file);
    EXPECT_TRUE(g_wake_found_the_name);

    read_listing(f);
    ASSERT_NULL(find_line(BOARD_NAME));
    unregister_detached_port(p);
}
