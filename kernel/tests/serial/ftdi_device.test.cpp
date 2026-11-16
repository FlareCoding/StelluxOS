#define STLX_TEST_TIER TIER_DRIVERS

#include "stlx_unit_test.h"
#include "runner.h"
#include "helpers.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"
#include "sync/poll.h"

TEST_SUITE(serial_ftdi_device);

// run_tests.sh gives QEMU's FTDI chip the serial number FT0001
static constexpr const char* PORT_PATH = "/dev/serial/ft232bm-0001";

// Plugging a device in and setting it up under emulation can take a few seconds
static constexpr uint64_t DEVICE_TIMEOUT_NS = 20000000000ULL;

// Bytes the host sends reach a port at once, so a read that waits longer has failed
static constexpr uint64_t READ_TIMEOUT_NS = 5000000000ULL;

// More than a packet holds, so reads carry several packets, each starting with status bytes the port must not see
static constexpr uint32_t MULTI_PACKET_BYTES = 200;

// Asks serial_test_host.py, which follows the serial log, to act as the far end of the chip's UART
static void ask_host(const char* request) {
    stlx_test::print("STLX_SERIAL_HOST %s", request);
}

static void ask_host_to_send(uint32_t first, uint32_t count) {
    stlx_test::print("STLX_SERIAL_HOST send %u %u", first, count);
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

TEST(serial_ftdi_device, a_port_reads_its_chips_bytes_until_the_chip_leaves) {
    ask_host("connect");
    ASSERT_TRUE(wait_until_listed(true));
    fs::file* f = fs::open(PORT_PATH, fs::O_RDONLY);
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
    fs::file* old_file = fs::open(PORT_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(old_file);

    ask_host("disconnect");
    ASSERT_TRUE(wait_until_listed(false));
    EXPECT_EQ(poll_events(old_file), sync::POLL_HUP);
    fs::close(old_file);

    // The second plug brings a new device and a new bridge driver, whose port must receive as the first one did
    ask_host("connect");
    ASSERT_TRUE(wait_until_listed(true));
    fs::file* new_file = fs::open(PORT_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(new_file);
    ask_host_to_send(MULTI_PACKET_BYTES, 1);
    EXPECT_TRUE(read_counting_bytes(new_file, MULTI_PACKET_BYTES, 1));

    fs::close(new_file);
}
