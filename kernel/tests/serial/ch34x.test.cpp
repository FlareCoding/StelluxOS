#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/serial/ch34x.h"
#include "terminal/tty.h"

TEST_SUITE(ch34x);

using namespace serial_test;
using drivers::xhci_hcd_test_access;
using serial::line_settings;
using serial::parity;
using usb::serial::ch34x;

struct speed_registers {
    uint32_t baud;
    uint16_t registers;
};

struct line_format {
    uint8_t data_bits;
    parity  mode;
    uint8_t stop_bits;
    uint8_t line_control;
};

struct modem_control {
    uint32_t lines;
    uint16_t value;
};

static constexpr uint8_t VENDOR_TO_DEVICE   = 0x40;
static constexpr uint8_t VENDOR_FROM_DEVICE = 0xC0;

static constexpr uint8_t READ_REGISTERS  = 0x95;
static constexpr uint8_t WRITE_REGISTERS = 0x9A;
static constexpr uint8_t RESET           = 0xA1;
static constexpr uint8_t MODEM_CONTROL   = 0xA4;

// Register pairs, the first register's address in the low byte
static constexpr uint16_t SPEED_REGISTERS        = 0x1312;
static constexpr uint16_t LINE_CONTROL_REGISTERS = 0x2518;
static constexpr uint16_t BREAK_REGISTERS        = 0x1805;
static constexpr uint16_t STATUS_REGISTERS       = 0x0706;
static constexpr uint16_t REGISTER_PAIR_LENGTH   = 2;

static constexpr uint16_t CH340_PRODUCT  = 0x7523;
static constexpr uint16_t CH340K_PRODUCT = 0x7522;
static constexpr uint16_t CH341_PRODUCT  = 0x5523;

// A current CH340, and the old release whose line control register differs
static constexpr uint16_t CH340_RELEASE     = 0x0254;
static constexpr uint16_t OLD_CH340_RELEASE = 0x0250;

// The speed registers, then the line control registers
static constexpr size_t SET_LINE_REQUESTS = 2;

// Static, so calls that never end still find them after their test, which then leaves the translator unfreed
static fake_chip s_chip;
static ch34x*    s_translator;

// What the calls take and give back
static line_settings s_settings;
static uint32_t      s_lines;
static bool          s_is_break_on;
static int32_t       s_rc;

// Builds a translator for the chip whose product ID is `product` and whose release is `release`
static bool attach_translator(uint16_t product = CH340_PRODUCT, uint16_t release = CH340_RELEASE) {
    if (!attach_fake_chip(s_chip)) {
        return false;
    }

    usb::device& dev = s_chip.controller->usb_device;
    dev.pid = product;
    dev.bcd_device = release;
    dev.num_interfaces = 1;
    s_translator = heap::ualloc_new<ch34x>(&dev, dev.interfaces[0]);
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

static void get_modem_inputs() {
    s_rc = s_translator->get_modem_inputs(&s_lines);
}

static void set_break() {
    s_rc = s_translator->set_break(s_is_break_on);
}

static void expect_request(const chip_request& request, uint8_t type, uint8_t code, uint16_t value, uint16_t index) {
    EXPECT_EQ(request.request_type, type);
    EXPECT_EQ(request.request, code);
    EXPECT_EQ(request.value, value);
    EXPECT_EQ(request.index, index);
    EXPECT_EQ(request.length, static_cast<uint16_t>(type == VENDOR_FROM_DEVICE ? REGISTER_PAIR_LENGTH : 0));
}

// The speed registers' values the translator writes for `baud` with 8 data bits, no parity and 1 stop bit, in a write
// that comes before the line control's
static uint16_t speed_registers_for(uint32_t baud) {
    chip_request requests[SET_LINE_REQUESTS];
    s_settings = {baud, 8, parity::none, 1, false};
    EXPECT_TRUE(make_calls(s_chip, set_line, requests, SET_LINE_REQUESTS));

    EXPECT_EQ(s_rc, serial::OK);
    EXPECT_EQ(requests[0].request_type, VENDOR_TO_DEVICE);
    EXPECT_EQ(requests[0].request, WRITE_REGISTERS);
    EXPECT_EQ(requests[0].value, SPEED_REGISTERS);

    return requests[0].index;
}

// True when the translator refuses `settings` without a request
static bool refuses(const line_settings& settings) {
    s_settings = settings;
    bool is_done = make_calls(s_chip, set_line, nullptr, 0);

    return is_done && s_rc == serial::ERR_UNSUPPORTED && sent_nothing(s_chip);
}

TEST(ch34x, configure_resets_the_chip) {
    ASSERT_TRUE(attach_translator());

    chip_request request;
    bool is_done = make_calls(s_chip, configure, &request, 1);

    detach_translator();
    ASSERT_TRUE(is_done);
    EXPECT_EQ(s_rc, serial::OK);
    expect_request(request, VENDOR_TO_DEVICE, RESET, 0, 0);
}

// The name a translator gives the chip whose product ID is `product`, or "" when the translator cannot be built
static const char* chip_name_for(uint16_t product) {
    if (!attach_translator(product)) {
        return "";
    }

    const char* name = s_translator->chip_name();
    detach_translator();
    return name;
}

// The chip reports no part number of its own
TEST(ch34x, the_product_id_names_the_chip) {
    EXPECT_STREQ(chip_name_for(CH340_PRODUCT), "ch340");
    EXPECT_STREQ(chip_name_for(CH340K_PRODUCT), "ch340k");
    EXPECT_STREQ(chip_name_for(CH341_PRODUCT), "ch341");
}

// 750 kHz by 78, 6 MHz by 52, 12 MHz by 13, 6 MHz by 3 and 11,719 Hz by 39, each with the prescaler's unbuffered bit
// 0x80 in the low byte, and 256 less the divisor in the high byte
static constexpr speed_registers COMMON_SPEEDS[] = {
    {9600,    0xB282},
    {115200,  0xCC83},
    {921600,  0xF387},
    {2000000, 0xFD83},
    {300,     0xD980},
};

TEST(ch34x, each_common_speed_takes_the_clock_and_divisor_nearest_it) {
    ASSERT_TRUE(attach_translator());

    for (const speed_registers& expected : COMMON_SPEEDS) {
        EXPECT_EQ(speed_registers_for(expected.baud), expected.registers);
    }

    detach_translator();
}

// 112,000 baud lies between 6 MHz by 53 and by 54, and the larger divisor's 111,111 baud is the nearer
TEST(ch34x, a_speed_between_two_divisors_takes_the_nearer) {
    ASSERT_TRUE(attach_translator());

    uint16_t registers = speed_registers_for(112000);

    detach_translator();
    EXPECT_EQ(registers, 0xCA83u);
}

// 107,143 baud is both 6 MHz by 56 and 750 kHz by 7, and the 6 MHz clock comes first
TEST(ch34x, a_tie_between_clocks_goes_to_the_clock_listed_first) {
    ASSERT_TRUE(attach_translator());

    uint16_t registers = speed_registers_for(107143);

    detach_translator();
    EXPECT_EQ(registers, 0xC883u);
}

// 2,000,000 baud is within 3 percent of 1,942,000 and 2,061,000 baud, and just outside it for 1,941,000 and 2,062,000.
// 576,000 baud is 4 percent from 6 MHz by 10, as 12 MHz makes 921,600 alone, and the slowest rate is 46 baud
TEST(ch34x, a_speed_more_than_3_percent_from_every_rate_is_refused_without_a_request) {
    ASSERT_TRUE(attach_translator());

    uint16_t just_below = speed_registers_for(1942000);
    uint16_t just_above = speed_registers_for(2061000);

    bool is_below_refused = refuses({1941000, 8, parity::none, 1, false});
    bool is_above_refused = refuses({2062000, 8, parity::none, 1, false});
    bool is_between_clocks_refused = refuses({576000, 8, parity::none, 1, false});
    bool is_too_fast_refused = refuses({3000000, 8, parity::none, 1, false});
    bool is_too_slow_refused = refuses({44, 8, parity::none, 1, false});

    detach_translator();
    EXPECT_EQ(just_below, 0xFD83u);
    EXPECT_EQ(just_above, 0xFD83u);

    EXPECT_TRUE(is_below_refused);
    EXPECT_TRUE(is_above_refused);
    EXPECT_TRUE(is_between_clocks_refused);
    EXPECT_TRUE(is_too_fast_refused);
    EXPECT_TRUE(is_too_slow_refused);
}

// Receiver and transmitter on in the top two bits, then fixed parity, even parity, parity on, two stop bits and the
// data bits less 5
static constexpr line_format LINE_FORMATS[] = {
    {8, parity::none,  1, 0xC3},
    {7, parity::even,  1, 0xDA},
    {5, parity::odd,   2, 0xCC},
    {6, parity::mark,  1, 0xE9},
    {8, parity::space, 1, 0xFB},
};

TEST(ch34x, the_line_control_register_holds_the_character_format) {
    ASSERT_TRUE(attach_translator());

    bool are_done = true;
    for (const line_format& format : LINE_FORMATS) {
        chip_request requests[SET_LINE_REQUESTS];
        s_settings = {9600, format.data_bits, format.mode, format.stop_bits, false};
        are_done = are_done && make_calls(s_chip, set_line, requests, SET_LINE_REQUESTS);

        EXPECT_EQ(s_rc, serial::OK);
        expect_request(requests[1], VENDOR_TO_DEVICE, WRITE_REGISTERS, LINE_CONTROL_REGISTERS, format.line_control);
    }

    detach_translator();
    EXPECT_TRUE(are_done);
}

// The old release takes the speed without the unbuffered bit, and its line control register keeps the chip's own 8N1
TEST(ch34x, an_old_ch340_takes_only_the_speed) {
    ASSERT_TRUE(attach_translator(CH340_PRODUCT, OLD_CH340_RELEASE));

    chip_request speed;
    s_settings = {115200, 8, parity::none, 1, false};
    bool is_done = make_calls(s_chip, set_line, &speed, 1);
    bool is_line_control_unwritten = sent_nothing(s_chip);

    detach_translator();
    ASSERT_TRUE(is_done);
    EXPECT_EQ(s_rc, serial::OK);
    expect_request(speed, VENDOR_TO_DEVICE, WRITE_REGISTERS, SPEED_REGISTERS, 0xCC03);
    EXPECT_TRUE(is_line_control_unwritten);
}

TEST(ch34x, an_old_ch340_refuses_every_format_but_8n1_without_a_request) {
    ASSERT_TRUE(attach_translator(CH340_PRODUCT, OLD_CH340_RELEASE));

    bool is_seven_bits_refused = refuses({115200, 7, parity::none, 1, false});
    bool is_parity_refused = refuses({115200, 8, parity::even, 1, false});
    bool is_two_stop_bits_refused = refuses({115200, 8, parity::none, 2, false});

    detach_translator();
    EXPECT_TRUE(is_seven_bits_refused);
    EXPECT_TRUE(is_parity_refused);
    EXPECT_TRUE(is_two_stop_bits_refused);
}

TEST(ch34x, flow_control_by_cts_is_refused_without_a_request) {
    ASSERT_TRUE(attach_translator());

    bool is_refused = refuses({115200, 8, parity::none, 1, true});

    detach_translator();
    EXPECT_TRUE(is_refused);
}

// DTR in bit 5 and RTS in bit 6, each clear while raised
static constexpr modem_control MODEM_CONTROLS[] = {
    {terminal::TIOCM_DTR | terminal::TIOCM_RTS, 0x9F},
    {terminal::TIOCM_DTR,                       0xDF},
    {terminal::TIOCM_RTS | terminal::TIOCM_CTS, 0xBF},
    {terminal::TIOCM_CAR | terminal::TIOCM_RNG, 0xFF},
};

TEST(ch34x, modem_control_sets_dtr_and_rts_in_one_request) {
    ASSERT_TRUE(attach_translator());

    bool are_done = true;
    for (const modem_control& expected : MODEM_CONTROLS) {
        chip_request request;
        s_lines = expected.lines;
        are_done = are_done && make_calls(s_chip, set_modem_outputs, &request, 1);

        EXPECT_EQ(s_rc, serial::OK);
        expect_request(request, VENDOR_TO_DEVICE, MODEM_CONTROL, expected.value, 0);
    }

    detach_translator();
    EXPECT_TRUE(are_done);
}

// CTS, DSR, RI and CD in the status register's low four bits, each clear while raised
TEST(ch34x, the_modem_status_register_gives_the_input_lines) {
    ASSERT_TRUE(attach_translator());

    chip_request every_line;
    uint8_t every_line_status[REGISTER_PAIR_LENGTH] = {0xF0, 0xEE};
    bool are_done = make_calls(s_chip, get_modem_inputs, &every_line, 1, every_line_status, REGISTER_PAIR_LENGTH);
    uint32_t every_input = s_lines;

    chip_request cts_only;
    uint8_t cts_status[REGISTER_PAIR_LENGTH] = {0xFE, 0x00};
    are_done = are_done && make_calls(s_chip, get_modem_inputs, &cts_only, 1, cts_status, REGISTER_PAIR_LENGTH);
    uint32_t cts_input = s_lines;

    chip_request no_line;
    uint8_t no_line_status[REGISTER_PAIR_LENGTH] = {0x0F, 0x00};
    are_done = are_done && make_calls(s_chip, get_modem_inputs, &no_line, 1, no_line_status, REGISTER_PAIR_LENGTH);
    uint32_t no_input = s_lines;

    detach_translator();
    ASSERT_TRUE(are_done);
    expect_request(every_line, VENDOR_FROM_DEVICE, READ_REGISTERS, STATUS_REGISTERS, 0);

    uint32_t inputs = terminal::TIOCM_CTS | terminal::TIOCM_DSR | terminal::TIOCM_RNG | terminal::TIOCM_CAR;
    EXPECT_EQ(every_input, inputs);
    EXPECT_EQ(cts_input, terminal::TIOCM_CTS);
    EXPECT_EQ(no_input, 0u);
}

// The break register's idle bit and the line control register's transmitter bit, read and written together
TEST(ch34x, a_break_clears_the_idle_and_transmitter_bits_and_keeps_the_rest) {
    ASSERT_TRUE(attach_translator());

    chip_request on[2];
    uint8_t idle[REGISTER_PAIR_LENGTH] = {0x5B, 0xDA};
    s_is_break_on = true;
    bool are_done = make_calls(s_chip, set_break, on, 2, idle, REGISTER_PAIR_LENGTH);

    chip_request off[2];
    uint8_t breaking[REGISTER_PAIR_LENGTH] = {0x5A, 0x9A};
    s_is_break_on = false;
    are_done = are_done && make_calls(s_chip, set_break, off, 2, breaking, REGISTER_PAIR_LENGTH);

    detach_translator();
    ASSERT_TRUE(are_done);
    expect_request(on[0], VENDOR_FROM_DEVICE, READ_REGISTERS, BREAK_REGISTERS, 0);
    expect_request(on[1], VENDOR_TO_DEVICE, WRITE_REGISTERS, BREAK_REGISTERS, 0x9A5A);
    expect_request(off[1], VENDOR_TO_DEVICE, WRITE_REGISTERS, BREAK_REGISTERS, 0xDA5B);
}

// A settings change during a break leaves the transmitter off, at 8N1's 0x83 rather than 0xC3, and ending the break
// turns it back on
TEST(ch34x, a_break_lasts_through_a_settings_change) {
    ASSERT_TRUE(attach_translator());

    chip_request on[2];
    uint8_t idle[REGISTER_PAIR_LENGTH] = {0x5B, 0xC3};
    s_is_break_on = true;
    bool are_done = make_calls(s_chip, set_break, on, 2, idle, REGISTER_PAIR_LENGTH);

    chip_request line[SET_LINE_REQUESTS];
    s_settings = {115200, 8, parity::none, 1, false};
    are_done = are_done && make_calls(s_chip, set_line, line, SET_LINE_REQUESTS);

    chip_request off[2];
    uint8_t breaking[REGISTER_PAIR_LENGTH] = {0x5A, 0x83};
    s_is_break_on = false;
    are_done = are_done && make_calls(s_chip, set_break, off, 2, breaking, REGISTER_PAIR_LENGTH);

    detach_translator();
    ASSERT_TRUE(are_done);
    expect_request(line[1], VENDOR_TO_DEVICE, WRITE_REGISTERS, LINE_CONTROL_REGISTERS, 0x0083);
    expect_request(off[1], VENDOR_TO_DEVICE, WRITE_REGISTERS, BREAK_REGISTERS, 0xC35B);
}

TEST(ch34x, the_bytes_left_to_send_are_unknown_without_a_request) {
    ASSERT_TRUE(attach_translator());

    ssize_t pending = s_translator->tx_pending();
    bool is_silent = sent_nothing(s_chip);

    detach_translator();
    EXPECT_EQ(pending, serial::TX_UNKNOWN);
    EXPECT_TRUE(is_silent);
}

// Once teardown has ended EP0's transfers, every request fails at once
TEST(ch34x, calls_after_an_unplug_fail_with_an_io_error) {
    ASSERT_TRUE(attach_translator());
    xhci_hcd_test_access::end_control_transfers(*s_chip.controller->hcd, s_chip.controller->device);

    bool are_done = make_calls(s_chip, configure, nullptr, 0);
    int32_t configure_rc = s_rc;

    s_settings = {115200, 8, parity::none, 1, false};
    are_done = are_done && make_calls(s_chip, set_line, nullptr, 0);
    int32_t line_rc = s_rc;

    s_lines = terminal::TIOCM_DTR | terminal::TIOCM_RTS;
    are_done = are_done && make_calls(s_chip, set_modem_outputs, nullptr, 0);
    int32_t outputs_rc = s_rc;

    are_done = are_done && make_calls(s_chip, get_modem_inputs, nullptr, 0);
    int32_t inputs_rc = s_rc;

    s_is_break_on = true;
    are_done = are_done && make_calls(s_chip, set_break, nullptr, 0);
    int32_t break_rc = s_rc;

    detach_translator();
    ASSERT_TRUE(are_done);

    EXPECT_EQ(configure_rc, serial::ERR_IO);
    EXPECT_EQ(line_rc, serial::ERR_IO);
    EXPECT_EQ(outputs_rc, serial::ERR_IO);
    EXPECT_EQ(inputs_rc, serial::ERR_IO);
    EXPECT_EQ(break_rc, serial::ERR_IO);
}
