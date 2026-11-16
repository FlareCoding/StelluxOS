#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/serial/ftdi.h"
#include "drivers/serial/serial.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(ftdi);

using namespace serial_test;
using usb::serial::ftdi;

static constexpr uint8_t VENDOR_TO_DEVICE = 0x40;
static constexpr uint8_t RESET = 0x00;
static constexpr uint16_t RESET_UART = 0x0000;

static constexpr uint16_t FT232BM_RELEASE = 0x0400;
static constexpr size_t STATUS_BYTES = 2;

// The packet sizes of a full-speed chip, such as the FT232R, and of a high-speed one, such as the FT2232H
static constexpr uint16_t FULL_SPEED_PACKET = 64;
static constexpr uint16_t HIGH_SPEED_PACKET = 512;
static constexpr uint16_t PACKET_SIZES[] = {FULL_SPEED_PACKET, HIGH_SPEED_PACKET};

// A board no other test names, for the port the translator hands its bytes to
static constexpr serial::identity BOARD = {
    .chip = "ftditest",
    .serial_number = "FT77",
    .location = "9.3",
    .channel = -1,
};

static constexpr const char* BOARD_PATH = "/dev/serial/ftditest-ft77";

// Static, so calls that never end still find them after their test, which then leaves the translator unfreed
static fake_chip s_chip;
static ftdi*     s_translator;
static int32_t   s_rc;

// Builds a translator for interface `number` of a chip with `interface_count` interfaces
static bool attach_translator(uint8_t interface_count, uint8_t number, uint16_t release = FT232BM_RELEASE) {
    if (!attach_fake_chip(s_chip)) {
        return false;
    }

    usb::device& dev = s_chip.controller->usb_device;
    dev.bcd_device = release;
    dev.num_interfaces = interface_count;
    for (uint8_t i = 0; i < interface_count; i++) {
        dev.interfaces[i].interface_number = i;
    }

    s_translator = heap::ualloc_new<ftdi>(&dev, dev.interfaces[number]);
    if (!s_translator) {
        detach_fake_chip(s_chip);
        return false;
    }

    return true;
}

static void detach_translator() {
    if (!s_chip.is_call_stuck) {
        heap::ufree_delete(s_translator);
    }

    detach_fake_chip(s_chip);
}

static void configure() {
    s_rc = s_translator->configure();
}

static void expect_reset(const chip_request& request, uint16_t uart) {
    EXPECT_EQ(request.request_type, VENDOR_TO_DEVICE);
    EXPECT_EQ(request.request, RESET);
    EXPECT_EQ(request.value, RESET_UART);
    EXPECT_EQ(request.index, uart);
    EXPECT_EQ(request.length, 0u);
}

TEST(ftdi, configure_resets_the_uart_and_names_the_chip) {
    ASSERT_TRUE(attach_translator(1, 0));

    chip_request request = {};
    EXPECT_TRUE(make_calls(s_chip, configure, &request, 1));
    EXPECT_EQ(s_rc, serial::OK);
    expect_reset(request, 1);
    EXPECT_TRUE(sent_nothing(s_chip));

    EXPECT_STREQ(s_translator->chip_name(), "ft232bm");
    EXPECT_EQ(s_translator->channel(), -1);

    detach_translator();
}

TEST(ftdi, each_uart_of_a_chip_with_several_is_a_channel) {
    ASSERT_TRUE(attach_translator(2, 1));

    chip_request request = {};
    EXPECT_TRUE(make_calls(s_chip, configure, &request, 1));
    expect_reset(request, 2);
    EXPECT_EQ(s_translator->channel(), 1);

    detach_translator();
}

// A release beside one the table holds, and one far from every entry
static constexpr uint16_t UNKNOWN_RELEASES[] = {0x0401, 0x9900};

TEST(ftdi, a_release_the_table_lacks_takes_the_family_name) {
    for (uint16_t release : UNKNOWN_RELEASES) {
        ASSERT_TRUE(attach_translator(1, 0, release));

        chip_request request = {};
        EXPECT_TRUE(make_calls(s_chip, configure, &request, 1));
        EXPECT_STREQ(s_translator->chip_name(), "ftdi");

        detach_translator();
    }
}

TEST(ftdi, a_setup_the_controller_refuses_fails) {
    ASSERT_TRUE(attach_translator(1, 0));
    RUN_ELEVATED(s_chip.controller->device->set_ctrl_gone());

    EXPECT_TRUE(make_calls(s_chip, configure, nullptr, 0));
    EXPECT_EQ(s_rc, serial::ERR_IO);

    detach_translator();
}

// Room for every read the stripping tests build
static constexpr size_t READ_ROOM = 3 * HIGH_SPEED_PACKET;

static uint8_t g_read_bytes[READ_ROOM];
static uint8_t g_expected[READ_ROOM];
static uint8_t g_received[READ_ROOM];

// Builds one read of packets that each hold `payloads[i]` bytes after the status bytes, and the bytes the port should
// see. Every payload but the last must fill its packet, since a short packet ends a read
template <size_t COUNT>
static void build_read(const size_t (&payloads)[COUNT], size_t* read_length, size_t* expected_length) {
    *read_length = 0;
    *expected_length = 0;
    for (size_t payload : payloads) {
        g_read_bytes[(*read_length)++] = 0x31;
        g_read_bytes[(*read_length)++] = 0x60;
        for (size_t i = 0; i < payload; i++) {
            uint8_t byte = static_cast<uint8_t>(*expected_length + 1);
            g_read_bytes[(*read_length)++] = byte;
            g_expected[(*expected_length)++] = byte;
        }
    }
}

// Hands `read_length` bytes of g_read_bytes to the translator and reads the port, true when it holds g_expected
static bool strips_to_expected(serial::port* p, fs::file* f, uint16_t packet_size, size_t read_length,
                               size_t expected_length) {
    s_translator->handle_received(p, g_read_bytes, read_length, packet_size);
    ssize_t n = fs::read(f, g_received, sizeof(g_received), fs::O_NONBLOCK);
    return n == static_cast<ssize_t>(expected_length) && string::memcmp(g_received, g_expected, expected_length) == 0;
}

TEST(ftdi, the_status_bytes_starting_each_packet_never_reach_the_port) {
    ASSERT_TRUE(attach_translator(1, 0));
    serial::port* p = serial::register_port(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    // At each packet size, two full packets and a short one, which ends the read, then a read ending on a full packet
    for (uint16_t packet_size : PACKET_SIZES) {
        size_t full = packet_size - STATUS_BYTES;
        size_t read_length = 0;
        size_t expected_length = 0;

        const size_t ends_short[] = {full, full, 9};
        build_read(ends_short, &read_length, &expected_length);
        EXPECT_TRUE(strips_to_expected(p, f, packet_size, read_length, expected_length));

        const size_t ends_full[] = {full, full};
        build_read(ends_full, &read_length, &expected_length);
        EXPECT_TRUE(strips_to_expected(p, f, packet_size, read_length, expected_length));
    }

    // With nothing received, the chip still sends its status bytes each time its latency timer runs out
    static constexpr uint8_t STATUS_ONLY[STATUS_BYTES] = {0x31, 0x60};
    s_translator->handle_received(p, STATUS_ONLY, sizeof(STATUS_ONLY), FULL_SPEED_PACKET);
    EXPECT_EQ(fs::read(f, g_received, sizeof(g_received), fs::O_NONBLOCK), static_cast<ssize_t>(fs::ERR_AGAIN));

    fs::close(f);
    serial::detach(p);
    serial::unregister_port(p);
    detach_translator();
}
