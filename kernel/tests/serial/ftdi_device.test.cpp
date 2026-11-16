#define STLX_TEST_TIER TIER_DRIVERS

#include "stlx_unit_test.h"
#include "runner.h"
#include "helpers.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "clock/clock.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sync/atomic.h"
#include "sync/poll.h"
#include "terminal/tty.h"

TEST_SUITE(serial_ftdi_device);

// run_tests.sh gives QEMU's FTDI chip the serial number FT0001
static constexpr const char* PORT_PATH = "/dev/serial/ft232bm-0001";

// Plugging a device in and setting it up under emulation can take a few seconds
static constexpr uint64_t DEVICE_TIMEOUT_NS = 20000000000ULL;

// Bytes the host sends reach a port at once, so a read that waits longer has failed
static constexpr uint64_t READ_TIMEOUT_NS = 5000000000ULL;

// More than a packet holds, so reads carry several packets, each starting with status bytes the port must not see
static constexpr uint32_t MULTI_PACKET_BYTES = 200;

// What serial_test_host.py sends back once QEMU's trace confirms what the test asked it to check
static constexpr uint8_t ACK = 0x06;

// A step a test runs on a task of its own, since opens and settings send chip requests, which wait for the controller,
// and a wait on the test runner's task never wakes
struct port_step {
    void                 (*run)();
    fs::file*              file;
    uint32_t               flags;
    int32_t                result;
    sync::atomic<uint32_t> is_done;
};

static port_step g_step;

// Asks serial_test_host.py, which follows the serial log, to act as the far end of the chip's UART
static void ask_host(const char* request) {
    stlx_test::print("STLX_SERIAL_HOST %s", request);
}

static void ask_host_to_send(uint32_t first, uint32_t count) {
    stlx_test::print("STLX_SERIAL_HOST send %u %u", first, count);
}

static void run_step_task(void*) {
    g_step.run();
    g_step.is_done.store_release(1);
    sched::exit(0);
}

// Runs `run` on an elevated task, as a program's call into the port runs, and waits for it
static bool run_step(void (*run)()) {
    g_step.run = run;
    g_step.is_done.store_release(0);
    sched::task* task = test_helpers::start_pinned_task_on_another_cpu(
        run_step_task,
        nullptr,
        "serial_device_step",
        sched::TASK_FLAG_ELEVATED
    );
    if (!task) {
        return false;
    }

    bool is_done = test_helpers::spin_wait(g_step.is_done);
    test_helpers::unpin(task);
    return is_done;
}

static void open_step() {
    g_step.file = fs::open(PORT_PATH, g_step.flags);
}

// TCGETS, then TCSETS at 115200 baud, through a user page as a program's requests carry their argument
static void set_speed_step() {
    test_helpers::user_page page;
    if (!page.ready()) {
        return;
    }

    test_helpers::user_space_scope scope(page.ctx);
    g_step.result = fs::ioctl(g_step.file, terminal::TCGETS, page.addr);
    if (g_step.result != fs::OK) {
        return;
    }

    auto* settings = page.at<terminal::termios>(0);
    settings->c_cflag = (settings->c_cflag & ~terminal::CBAUD) | terminal::B115200;
    g_step.result = fs::ioctl(g_step.file, terminal::TCSETS, page.addr);
}

static fs::file* open_port(uint32_t flags) {
    g_step.file = nullptr;
    g_step.flags = flags;
    return run_step(open_step) ? g_step.file : nullptr;
}

static int32_t set_speed_115200(fs::file* f) {
    g_step.file = f;
    g_step.result = fs::ERR_IO;
    return run_step(set_speed_step) ? g_step.result : fs::ERR_IO;
}

static bool is_listed() {
    fs::vattr attr = {};
    return fs::stat(PORT_PATH, &attr) == fs::OK;
}

static bool wait_until_listed(bool should_be_listed) {
    uint64_t deadline = clock::now_ns() + DEVICE_TIMEOUT_NS;
    while (is_listed() != should_be_listed) {
        if (clock::now_ns() > deadline) {
            return false;
        }

        test_helpers::brief_delay();
    }

    return true;
}

// Unplugs the chip after every test, so a test an ASSERT ended still leaves the next one a chip to plug in
static int32_t unplug_chip() {
    ask_host("disconnect");
    return wait_until_listed(false) ? 0 : -1;
}

AFTER_EACH(serial_ftdi_device, unplug_chip);

static uint32_t poll_events(fs::file* f) {
    uint32_t events = 0;
    RUN_ELEVATED(events = f->get_node()->poll(f, nullptr));
    return events;
}

static bool wait_until_readable(fs::file* f) {
    uint64_t deadline = clock::now_ns() + READ_TIMEOUT_NS;
    while (!(poll_events(f) & sync::POLL_IN)) {
        if (clock::now_ns() > deadline) {
            return false;
        }

        test_helpers::brief_delay();
    }

    return true;
}

// True when `count` bytes counting up from `first` arrive in order, read without blocking, since a wait on the test
// runner's task never wakes
static bool read_counting_bytes(fs::file* f, uint32_t first, uint32_t count) {
    uint8_t received[MULTI_PACKET_BYTES] = {};
    size_t total = 0;
    uint64_t deadline = clock::now_ns() + READ_TIMEOUT_NS;
    while (total < count && clock::now_ns() < deadline) {
        ssize_t n = fs::read(f, received + total, count - total, fs::O_NONBLOCK);
        if (n > 0) {
            total += static_cast<size_t>(n);
        } else if (n == fs::ERR_AGAIN) {
            test_helpers::brief_delay();
        } else {
            return false;
        }
    }

    for (size_t i = 0; i < total; i++) {
        if (received[i] != static_cast<uint8_t>(first + i)) {
            return false;
        }
    }

    return total == count;
}

static constexpr const char* LISTING_PATH = "/dev/sysinfo/serial";
static constexpr size_t LISTING_BYTES = 4096;

// The chip's fields in the listing up to its location, which depends on the port QEMU plugs it into
static constexpr const char* LISTED_CHIP = "ft232bm-0001\tusb\tftdi\t0403:6001\t";

static char g_listing[LISTING_BYTES];

// Takes a new snapshot of the listing, true when a line of it starts with LISTED_CHIP
static bool listing_shows_chip(fs::file* listing) {
    if (fs::seek(listing, 0, fs::SEEK_SET) != 0) {
        return false;
    }

    size_t length = 0;
    ssize_t n = fs::read(listing, g_listing, sizeof(g_listing) - 1);
    while (n > 0) {
        length += static_cast<size_t>(n);
        n = fs::read(listing, g_listing + length, sizeof(g_listing) - 1 - length);
    }

    g_listing[length] = '\0';
    size_t prefix_length = string::strlen(LISTED_CHIP);
    for (size_t line = 0; line < length; line++) {
        bool starts_line = line == 0 || g_listing[line - 1] == '\n';
        if (starts_line && string::strncmp(g_listing + line, LISTED_CHIP, prefix_length) == 0) {
            return true;
        }
    }

    return false;
}

TEST(serial_ftdi_device, a_port_reads_its_chips_bytes_until_the_chip_leaves) {
    ask_host("connect");
    ASSERT_TRUE(wait_until_listed(true));
    fs::file* f = open_port(fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    ask_host_to_send(0, MULTI_PACKET_BYTES);
    EXPECT_TRUE(read_counting_bytes(f, 0, MULTI_PACKET_BYTES));

    // A one-byte send is queued in full once poll reports it, so the unplug that follows finds it queued
    ask_host_to_send(MULTI_PACKET_BYTES, 1);
    ASSERT_TRUE(wait_until_readable(f));
    ask_host("disconnect");
    ASSERT_TRUE(wait_until_listed(false));

    uint8_t last = 0;
    EXPECT_TRUE(read_counting_bytes(f, MULTI_PACKET_BYTES, 1));
    EXPECT_EQ(fs::read(f, &last, 1, fs::O_NONBLOCK), 0);
    EXPECT_EQ(poll_events(f), sync::POLL_HUP);

    fs::close(f);
}

TEST(serial_ftdi_device, a_chip_that_comes_back_gets_its_name_and_its_bytes_again) {
    ask_host("connect");
    ASSERT_TRUE(wait_until_listed(true));
    fs::file* old_file = open_port(fs::O_RDONLY);
    ASSERT_NOT_NULL(old_file);

    ask_host("disconnect");
    ASSERT_TRUE(wait_until_listed(false));
    EXPECT_EQ(poll_events(old_file), sync::POLL_HUP);
    fs::close(old_file);

    // The second plug brings a new device and a new bridge driver, whose port must receive as the first one did
    ask_host("connect");
    ASSERT_TRUE(wait_until_listed(true));
    fs::file* new_file = open_port(fs::O_RDONLY);
    ASSERT_NOT_NULL(new_file);
    ask_host_to_send(MULTI_PACKET_BYTES, 1);
    EXPECT_TRUE(read_counting_bytes(new_file, MULTI_PACKET_BYTES, 1));

    fs::close(new_file);
}

static fs::file* g_listing_poller_file;
static sync::atomic<uint32_t> g_listing_poller_is_woken;
static sync::atomic<uint32_t> g_listing_poller_done;

// Sleeps in a poll of the listing on a task of its own, since the test runner's task cannot sleep
static void poll_listing(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        uint32_t events = g_listing_poller_file->get_node()->poll(g_listing_poller_file, &pt);
        bool is_woken = (events & sync::POLL_IN) == 0 && sync::poll_wait(pt, DEVICE_TIMEOUT_NS);
        g_listing_poller_is_woken.store_relaxed(is_woken ? 1 : 0);
        sync::poll_cleanup(pt);
    });

    g_listing_poller_done.store_release(1);
    sched::exit(0);
}

static sched::task* start_listing_poller(fs::file* listing) {
    g_listing_poller_file = listing;
    g_listing_poller_is_woken.store_relaxed(0);
    g_listing_poller_done.store_relaxed(0);
    return test_helpers::start_pinned_task_on_another_cpu(poll_listing, nullptr, "listing_poller", 0);
}

// True once the poller has woken, or has given up after DEVICE_TIMEOUT_NS, so the listing file outlives its poll
static bool wait_for_listing_poller() {
    uint64_t deadline = clock::now_ns() + 2 * DEVICE_TIMEOUT_NS;
    while (!g_listing_poller_done.load_acquire()) {
        if (clock::now_ns() > deadline) {
            return false;
        }

        test_helpers::brief_delay();
    }

    return g_listing_poller_is_woken.load_relaxed() == 1;
}

TEST(serial_ftdi_device, a_poller_of_the_listing_wakes_as_the_chip_comes_and_goes) {
    fs::file* listing = fs::open(LISTING_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(listing);
    EXPECT_FALSE(listing_shows_chip(listing));

    sched::task* poller = start_listing_poller(listing);
    ASSERT_NOT_NULL(poller);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(poller));

    ask_host("connect");
    ASSERT_TRUE(wait_for_listing_poller());
    test_helpers::unpin(poller);
    EXPECT_TRUE(listing_shows_chip(listing));

    poller = start_listing_poller(listing);
    ASSERT_NOT_NULL(poller);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(poller));

    ask_host("disconnect");
    ASSERT_TRUE(wait_for_listing_poller());
    test_helpers::unpin(poller);
    EXPECT_FALSE(listing_shows_chip(listing));

    fs::close(listing);
}

// A socket has no line settings, so the far end confirms the speed from QEMU's trace of the chip's requests
TEST(serial_ftdi_device, a_speed_set_through_termios_reaches_the_chip) {
    ask_host("connect");
    ASSERT_TRUE(wait_until_listed(true));
    fs::file* f = open_port(fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    EXPECT_EQ(set_speed_115200(f), fs::OK);
    stlx_test::print("STLX_SERIAL_HOST baud %u", 115200);
    EXPECT_TRUE(wait_until_readable(f));

    uint8_t answer = 0;
    EXPECT_EQ(fs::read(f, &answer, 1, fs::O_NONBLOCK), 1);
    EXPECT_EQ(answer, ACK);

    fs::close(f);
}
