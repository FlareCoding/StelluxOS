#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/serial/ftdi.h"
#include "drivers/serial/serial.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "dynpriv/dynpriv.h"
#include "tests/serial/fake_port_driver.h"
#include "terminal/tty.h"

TEST_SUITE(ftdi);

using namespace serial_test;
using usb::serial::ftdi;

static constexpr uint8_t VENDOR_TO_DEVICE = 0x40;
static constexpr uint8_t VENDOR_FROM_DEVICE = 0xC0;
static constexpr uint8_t RESET = 0x00;
static constexpr uint16_t RESET_UART = 0x0000;

static constexpr uint8_t SET_MODEM_CTRL = 0x01;
static constexpr uint8_t SET_FLOW_CTRL = 0x02;
static constexpr uint8_t SET_BAUD_RATE = 0x03;
static constexpr uint8_t SET_DATA = 0x04;
static constexpr uint8_t GET_MODEM_STATUS = 0x05;
static constexpr uint8_t SET_LATENCY_TIMER = 0x09;
static constexpr uint16_t LATENCY_TIMER_MS = 1;
static constexpr size_t SET_LINE_REQUESTS = 3;

static constexpr uint16_t FT232AM_RELEASE = 0x0200;
static constexpr uint16_t FT232BM_RELEASE = 0x0400;
static constexpr uint16_t FT2232H_RELEASE = 0x0700;
static constexpr size_t STATUS_BYTES = 2;

// The packet sizes of a full-speed chip, such as the FT232R, and of a high-speed one, such as the FT2232H
static constexpr uint16_t FULL_SPEED_PACKET = 64;
static constexpr uint16_t HIGH_SPEED_PACKET = 512;
static constexpr uint16_t PACKET_SIZES[] = {FULL_SPEED_PACKET, HIGH_SPEED_PACKET};

// The driver of every port the tests register, whose chip answers at once
static serial_test::fake_port_driver s_driver;

// A board no other test names, for the port the translator hands its bytes to
static constexpr serial::identity BOARD = {
    .chip = "ftditest",
    .serial_number = "FT77",
    .location = "9.3",
    .channel = -1,
};

static constexpr const char* BOARD_PATH = "/dev/serial/ftditest-ft77";

struct baud_divisor {
    uint32_t baud;
    uint16_t value;
    uint16_t index;
};

struct data_format {
    uint8_t        data_bits;
    serial::parity mode;
    uint8_t        stop_bits;
    uint16_t       value;
};

// The divisor's whole part in bits 0 to 13 and its eighths, coded, in bits 14 to 16, whose top bit goes in the index.
// 3 and 2 Mbaud have codes of their own
static constexpr baud_divisor BAUD_DIVISORS[] = {
    {115200,  26,     0},
    {57600,   0xC034, 0},
    {9600,    0x4138, 0},
    {921600,  0x8003, 0},
    {76190,   0x0027, 1},
    {183,     0xFFFF, 1},
    {3000000, 0,      0},
    {2915000, 0,      0},
    {2000000, 1,      0},
};

// Data bits in the low byte, then parity in bits 8 to 10 and stop bits in bits 11 to 13
static constexpr data_format DATA_FORMATS[] = {
    {8, serial::parity::none,  1, 0x0008},
    {7, serial::parity::even,  2, 0x1207},
    {8, serial::parity::odd,   1, 0x0108},
    {8, serial::parity::mark,  1, 0x0308},
    {7, serial::parity::space, 1, 0x0407},
};

// Rates no divisor makes within 3 percent: just past 3 percent from 3 Mbaud, between 3 and 2 Mbaud, between 2 and 1.5
// Mbaud, below the slowest, and 0
static constexpr uint32_t UNMADE_RATES[] = {2910000, 2500000, 1900000, 170, 0};

// Static, so calls that never end still find them after their test, which then leaves the translator unfreed
static fake_chip             s_chip;
static ftdi*                 s_translator;
static int32_t               s_rc;
static serial::line_settings s_settings;
static uint32_t              s_lines;
static bool                  s_is_on;
static ssize_t               s_pending;

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

static void set_line() {
    s_rc = s_translator->set_line(s_settings);
}

static void set_modem_outputs() {
    s_rc = s_translator->set_modem_outputs(s_lines);
}

static void set_break() {
    s_rc = s_translator->set_break(s_is_on);
}

static void tx_pending() {
    s_pending = s_translator->tx_pending();
}

static serial::line_settings settings_of(uint32_t baud, uint8_t data_bits, serial::parity mode, uint8_t stop_bits) {
    return {baud, data_bits, mode, stop_bits, false};
}

// Configures the translator, answering its one request
static bool configure_translator() {
    chip_request reset = {};
    return make_calls(s_chip, configure, &reset, 1);
}

// Configures the translator and makes the first open's set_line, which sends the latency timer before the line's
// three requests, so a test's own set_line sends those three alone
static bool opened() {
    if (!configure_translator()) {
        return false;
    }

    s_settings = settings_of(9600, 8, serial::parity::none, 1);
    chip_request first[SET_LINE_REQUESTS + 1];
    return make_calls(s_chip, set_line, first, SET_LINE_REQUESTS + 1);
}

// Opens the translator as `opened` does, then sets `settings`, answering its three requests and recording them in
// `requests`
static bool set_line_after_open(const serial::line_settings& settings, chip_request* requests) {
    if (!opened()) {
        return false;
    }

    s_settings = settings;
    return make_calls(s_chip, set_line, requests, SET_LINE_REQUESTS);
}

// True when the translator refuses `settings` without a request
static bool refuses(const serial::line_settings& settings) {
    s_settings = settings;
    bool is_done = make_calls(s_chip, set_line, nullptr, 0);

    return is_done && s_rc == serial::ERR_UNSUPPORTED && sent_nothing(s_chip);
}

static void expect_request(const chip_request& request, uint8_t code, uint16_t value, uint16_t index) {
    EXPECT_EQ(request.request_type, VENDOR_TO_DEVICE);
    EXPECT_EQ(request.request, code);
    EXPECT_EQ(request.value, value);
    EXPECT_EQ(request.index, index);
    EXPECT_EQ(request.length, 0u);
}

static void expect_reset(const chip_request& request, uint16_t uart) {
    expect_request(request, RESET, RESET_UART, uart);
}

TEST(ftdi, configure_resets_the_uart_and_names_the_chip) {
    ASSERT_TRUE(attach_translator(1, 0));

    chip_request request = {};
    EXPECT_TRUE(make_calls(s_chip, configure, &request, 1));
    EXPECT_EQ(s_rc, serial::OK);
    expect_reset(request, 1);
    EXPECT_TRUE(sent_nothing(s_chip));

    EXPECT_STREQ(s_translator->chip_name(), "ft232bm");
    EXPECT_STREQ(s_translator->driver_name(), "ftdi");
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
    serial::port* p = serial::register_port(&s_driver, BOARD);
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

// A chip with one UART names it in every index but SET_BAUD_RATE's, which carries the divisor's top bit
TEST(ftdi, set_line_sends_the_divisor_then_the_format_then_flow_control) {
    ASSERT_TRUE(attach_translator(1, 0));

    chip_request requests[SET_LINE_REQUESTS];
    EXPECT_TRUE(set_line_after_open(settings_of(115200, 8, serial::parity::none, 1), requests));
    EXPECT_EQ(s_rc, serial::OK);
    expect_request(requests[0], SET_BAUD_RATE, 26, 0);
    expect_request(requests[1], SET_DATA, 0x0008, 1);
    expect_request(requests[2], SET_FLOW_CTRL, 0, 1);

    detach_translator();
}

TEST(ftdi, set_baud_rate_codes_the_divisor_of_the_nearest_rate) {
    ASSERT_TRUE(attach_translator(1, 0));

    EXPECT_TRUE(opened());
    for (const baud_divisor& divisor : BAUD_DIVISORS) {
        chip_request requests[SET_LINE_REQUESTS];
        s_settings = settings_of(divisor.baud, 8, serial::parity::none, 1);
        EXPECT_TRUE(make_calls(s_chip, set_line, requests, SET_LINE_REQUESTS));

        EXPECT_EQ(s_rc, serial::OK);
        expect_request(requests[0], SET_BAUD_RATE, divisor.value, divisor.index);
    }

    detach_translator();
}

TEST(ftdi, a_rate_no_divisor_makes_within_3_percent_is_refused) {
    ASSERT_TRUE(attach_translator(1, 0));

    EXPECT_TRUE(opened());
    for (uint32_t baud : UNMADE_RATES) {
        EXPECT_TRUE(refuses(settings_of(baud, 8, serial::parity::none, 1)));
    }

    detach_translator();
}

TEST(ftdi, set_data_encodes_each_format_of_7_or_8_data_bits) {
    ASSERT_TRUE(attach_translator(1, 0));

    EXPECT_TRUE(opened());
    for (const data_format& format : DATA_FORMATS) {
        chip_request requests[SET_LINE_REQUESTS];
        s_settings = settings_of(9600, format.data_bits, format.mode, format.stop_bits);
        EXPECT_TRUE(make_calls(s_chip, set_line, requests, SET_LINE_REQUESTS));

        EXPECT_EQ(s_rc, serial::OK);
        expect_request(requests[1], SET_DATA, format.value, 1);
    }

    EXPECT_TRUE(refuses(settings_of(9600, 6, serial::parity::none, 1)));
    EXPECT_TRUE(refuses(settings_of(9600, 5, serial::parity::none, 1)));

    detach_translator();
}

TEST(ftdi, rts_cts_flow_control_sets_the_index_high_byte) {
    ASSERT_TRUE(attach_translator(1, 0));

    chip_request requests[SET_LINE_REQUESTS];
    EXPECT_TRUE(set_line_after_open({115200, 8, serial::parity::none, 1, true}, requests));
    expect_request(requests[2], SET_FLOW_CTRL, 0, 0x0101);

    detach_translator();
}

// A chip with several UARTs names the UART in SET_BAUD_RATE's index too, with the divisor's top bit a byte higher
TEST(ftdi, a_chip_with_several_uarts_names_the_uart_in_set_baud_rate_too) {
    ASSERT_TRUE(attach_translator(2, 1, FT2232H_RELEASE));

    chip_request requests[SET_LINE_REQUESTS];
    EXPECT_TRUE(set_line_after_open(settings_of(115200, 8, serial::parity::none, 1), requests));
    expect_request(requests[0], SET_BAUD_RATE, 26, 0x0002);
    expect_request(requests[1], SET_DATA, 0x0008, 2);
    expect_request(requests[2], SET_FLOW_CTRL, 0, 2);

    chip_request slowest[SET_LINE_REQUESTS];
    s_settings = settings_of(183, 8, serial::parity::none, 1);
    EXPECT_TRUE(make_calls(s_chip, set_line, slowest, SET_LINE_REQUESTS));
    expect_request(slowest[0], SET_BAUD_RATE, 0xFFFF, 0x0102);

    detach_translator();
}

// The FT232AM adds only none, one, two or four eighths to a whole divisor, and never divides by 1.5
TEST(ftdi, an_ft232am_refuses_the_divisors_it_lacks) {
    ASSERT_TRUE(attach_translator(1, 0, FT232AM_RELEASE));

    chip_request requests[SET_LINE_REQUESTS];
    EXPECT_TRUE(set_line_after_open(settings_of(9600, 8, serial::parity::none, 1), requests));
    expect_request(requests[0], SET_BAUD_RATE, 0x4138, 0);
    EXPECT_TRUE(refuses(settings_of(76190, 8, serial::parity::none, 1)));
    EXPECT_TRUE(refuses(settings_of(2000000, 8, serial::parity::none, 1)));

    detach_translator();
}

// The first open's set_line shortens the latency timer before the line's requests, and later calls leave it
TEST(ftdi, the_first_open_shortens_the_latency_timer_once) {
    ASSERT_TRUE(attach_translator(1, 0));
    ASSERT_TRUE(configure_translator());

    chip_request first[SET_LINE_REQUESTS + 1];
    s_settings = settings_of(115200, 8, serial::parity::none, 1);
    EXPECT_TRUE(make_calls(s_chip, set_line, first, SET_LINE_REQUESTS + 1));
    EXPECT_EQ(s_rc, serial::OK);
    expect_request(first[0], SET_LATENCY_TIMER, LATENCY_TIMER_MS, 1);
    expect_request(first[1], SET_BAUD_RATE, 26, 0);

    chip_request later[SET_LINE_REQUESTS];
    EXPECT_TRUE(make_calls(s_chip, set_line, later, SET_LINE_REQUESTS));
    expect_request(later[0], SET_BAUD_RATE, 26, 0);
    EXPECT_TRUE(sent_nothing(s_chip));

    detach_translator();
}

// A first set_line the chip cannot make sends nothing, the latency timer included, which the next one sends
TEST(ftdi, a_refused_first_set_line_leaves_the_latency_timer_to_the_next) {
    ASSERT_TRUE(attach_translator(1, 0));
    ASSERT_TRUE(configure_translator());
    EXPECT_TRUE(refuses(settings_of(9600, 6, serial::parity::none, 1)));

    chip_request first[SET_LINE_REQUESTS + 1];
    s_settings = settings_of(9600, 8, serial::parity::none, 1);
    EXPECT_TRUE(make_calls(s_chip, set_line, first, SET_LINE_REQUESTS + 1));
    expect_request(first[0], SET_LATENCY_TIMER, LATENCY_TIMER_MS, 1);

    detach_translator();
}

// The oldest chips may refuse the latency timer, which the translator logs and passes over to set the line
TEST(ftdi, a_refused_latency_timer_still_lets_the_line_be_set) {
    ASSERT_TRUE(attach_translator(1, 0, FT232AM_RELEASE));
    ASSERT_TRUE(configure_translator());

    s_settings = settings_of(9600, 8, serial::parity::none, 1);
    sched::task* calls = start_calls(s_chip, set_line);
    ASSERT_NOT_NULL(calls);

    chip_request latency = {};
    chip_request requests[SET_LINE_REQUESTS] = {};
    bool is_answered = refuse_request(s_chip, latency);
    for (size_t i = 0; i < SET_LINE_REQUESTS && is_answered; i++) {
        is_answered = answer_request(s_chip, requests[i], nullptr, 0);
    }

    EXPECT_TRUE(finish_calls(s_chip, calls));
    EXPECT_TRUE(is_answered);
    EXPECT_EQ(s_rc, serial::OK);

    expect_request(latency, SET_LATENCY_TIMER, LATENCY_TIMER_MS, 1);
    expect_request(requests[0], SET_BAUD_RATE, 0x4138, 0);
    expect_request(requests[1], SET_DATA, 0x0008, 1);
    expect_request(requests[2], SET_FLOW_CTRL, 0, 1);

    detach_translator();
}

// SET_MODEM_CTRL sets each line with the bit that lets it change, for both lines at once
TEST(ftdi, modem_outputs_set_both_lines_with_their_write_bits) {
    ASSERT_TRUE(attach_translator(1, 0));
    ASSERT_TRUE(opened());

    const uint32_t lines[] = {terminal::TIOCM_DTR | terminal::TIOCM_RTS, terminal::TIOCM_DTR, 0};
    const uint16_t values[] = {0x0303, 0x0301, 0x0300};
    for (size_t i = 0; i < 3; i++) {
        chip_request request = {};
        s_lines = lines[i];
        EXPECT_TRUE(make_calls(s_chip, set_modem_outputs, &request, 1));
        EXPECT_EQ(s_rc, serial::OK);
        expect_request(request, SET_MODEM_CTRL, values[i], 1);
    }

    detach_translator();
}

// The modem status byte starting each packet gives the inputs, so reading them sends nothing
TEST(ftdi, modem_inputs_come_from_the_latest_packets_status_byte) {
    ASSERT_TRUE(attach_translator(1, 0));
    ASSERT_TRUE(opened());

    static constexpr uint8_t CTS_AND_DSR[STATUS_BYTES] = {0x31, 0x60};
    static constexpr uint8_t RI_AND_CD[STATUS_BYTES] = {0xC1, 0x60};
    uint32_t inputs = 0;
    s_translator->handle_received(nullptr, CTS_AND_DSR, sizeof(CTS_AND_DSR), FULL_SPEED_PACKET);
    EXPECT_EQ(s_translator->get_modem_inputs(&inputs), serial::OK);
    EXPECT_EQ(inputs, terminal::TIOCM_CTS | terminal::TIOCM_DSR);

    s_translator->handle_received(nullptr, RI_AND_CD, sizeof(RI_AND_CD), FULL_SPEED_PACKET);
    EXPECT_EQ(s_translator->get_modem_inputs(&inputs), serial::OK);
    EXPECT_EQ(inputs, terminal::TIOCM_RNG | terminal::TIOCM_CAR);
    EXPECT_TRUE(sent_nothing(s_chip));

    detach_translator();
}

// A read of several packets leaves the inputs its last packet reports, the latest the chip has given
TEST(ftdi, modem_inputs_come_from_the_last_packet_of_a_read) {
    ASSERT_TRUE(attach_translator(1, 0));
    serial::port* p = serial::register_port(&s_driver, BOARD);
    ASSERT_NOT_NULL(p);

    size_t read_length = 0;
    size_t expected_length = 0;
    const size_t payloads[] = {FULL_SPEED_PACKET - STATUS_BYTES, 0};
    build_read(payloads, &read_length, &expected_length);
    g_read_bytes[FULL_SPEED_PACKET] = 0xC1;

    uint32_t inputs = 0;
    s_translator->handle_received(p, g_read_bytes, read_length, FULL_SPEED_PACKET);
    EXPECT_EQ(s_translator->get_modem_inputs(&inputs), serial::OK);
    EXPECT_EQ(inputs, terminal::TIOCM_RNG | terminal::TIOCM_CAR);

    serial::detach(p);
    serial::unregister_port(p);
    detach_translator();
}

// A break sets SET_DATA's bit 14 in the format set_line last sent, and a later set_line keeps it while the break lasts
TEST(ftdi, a_break_sets_a_bit_in_the_character_format) {
    ASSERT_TRUE(attach_translator(1, 0));

    chip_request line[SET_LINE_REQUESTS];
    EXPECT_TRUE(set_line_after_open(settings_of(9600, 7, serial::parity::even, 2), line));

    chip_request request = {};
    s_is_on = true;
    EXPECT_TRUE(make_calls(s_chip, set_break, &request, 1));
    EXPECT_EQ(s_rc, serial::OK);
    expect_request(request, SET_DATA, 0x5207, 1);

    s_settings = settings_of(9600, 8, serial::parity::none, 1);
    EXPECT_TRUE(make_calls(s_chip, set_line, line, SET_LINE_REQUESTS));
    expect_request(line[1], SET_DATA, 0x4008, 1);

    s_is_on = false;
    EXPECT_TRUE(make_calls(s_chip, set_break, &request, 1));
    expect_request(request, SET_DATA, 0x0008, 1);

    detach_translator();
}

// GET_MODEM_STATUS tells only whether the transmitter is empty, so until it is the chip may hold its whole buffer
TEST(ftdi, tx_pending_reports_the_transmit_buffer_until_the_transmitter_is_empty) {
    ASSERT_TRUE(attach_translator(2, 1, FT2232H_RELEASE));
    ASSERT_TRUE(configure_translator());

    static constexpr uint8_t SENDING[2] = {0x31, 0x20};
    static constexpr uint8_t EMPTY[2] = {0x31, 0x60};
    chip_request request = {};
    EXPECT_TRUE(make_calls(s_chip, tx_pending, &request, 1, SENDING, sizeof(SENDING)));
    EXPECT_EQ(s_pending, static_cast<ssize_t>(4096));

    EXPECT_EQ(request.request_type, VENDOR_FROM_DEVICE);
    EXPECT_EQ(request.request, GET_MODEM_STATUS);
    EXPECT_EQ(request.index, 2u);
    EXPECT_EQ(request.length, 2u);

    EXPECT_TRUE(make_calls(s_chip, tx_pending, &request, 1, EMPTY, sizeof(EMPTY)));
    EXPECT_EQ(s_pending, static_cast<ssize_t>(0));

    detach_translator();
}
