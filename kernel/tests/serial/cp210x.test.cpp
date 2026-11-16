#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "tests/serial/fake_chip.h"
#include "drivers/usb/serial/cp210x.h"
#include "drivers/serial/serial.h"
#include "dynpriv/dynpriv.h"
#include "terminal/tty.h"

TEST_SUITE(cp210x);

using namespace serial_test;
using serial::line_settings;
using serial::parity;
using usb::serial::cp210x;

struct character_format {
    uint8_t  data_bits;
    parity   mode;
    uint8_t  stop_bits;
    uint16_t line_control;
};

struct modem_outputs {
    uint32_t lines;
    uint16_t value;
};

struct modem_status {
    uint8_t  status;
    uint32_t inputs;
};

struct flow_words {
    uint32_t lines;
    uint32_t handshake;
    uint32_t rts_mode;
};

static constexpr uint8_t VENDOR_TO_INTERFACE = 0x41;
static constexpr uint8_t VENDOR_FROM_INTERFACE = 0xC1;

static constexpr uint8_t IFC_ENABLE = 0x00;
static constexpr uint8_t SET_LINE_CTL = 0x03;
static constexpr uint8_t SET_BREAK = 0x05;
static constexpr uint8_t SET_MHS = 0x07;
static constexpr uint8_t GET_MDMSTS = 0x08;
static constexpr uint8_t GET_COMM_STATUS = 0x10;
static constexpr uint8_t SET_FLOW = 0x13;
static constexpr uint8_t SET_BAUDRATE = 0x1E;
static constexpr uint8_t VENDOR_SPECIFIC = 0xFF;
static constexpr uint16_t GET_PART_NUMBER = 0x370B;

static constexpr uint8_t CP2102 = 0x02;
static constexpr uint8_t CP2104 = 0x04;
static constexpr uint8_t CP2102N = 0x20;
static constexpr uint8_t UNLISTED_PART = 0x77;

// The second of two interfaces, so a request that names interface 0 fails the check
static constexpr uint8_t INTERFACE_NUMBER = 1;

static constexpr uint16_t RATE_LENGTH = 4;
static constexpr uint16_t FLOW_LENGTH = 16;
static constexpr uint16_t COMM_STATUS_LENGTH = 19;
static constexpr size_t BYTES_TO_SEND_OFFSET = 12;

// The part number request, then IFC_ENABLE
static constexpr size_t CONFIGURE_REQUESTS = 2;

// SET_BAUDRATE, SET_LINE_CTL and SET_FLOW
static constexpr size_t SET_LINE_REQUESTS = 3;

// Data bits in the high byte, then parity in bits 4 to 7 and stop bits in bits 0 to 3
static constexpr character_format CHARACTER_FORMATS[] = {
    {7, parity::even,  2, 0x0722},
    {5, parity::odd,   1, 0x0510},
    {8, parity::mark,  1, 0x0830},
    {6, parity::space, 2, 0x0642},
};

// The levels in bits 0 and 1, and in bits 8 and 9 the bits that let the request change them
static constexpr modem_outputs MODEM_OUTPUTS[] = {
    {terminal::TIOCM_DTR | terminal::TIOCM_RTS, 0x0303},
    {terminal::TIOCM_DTR,                       0x0301},
    {terminal::TIOCM_RTS,                       0x0302},
    {0,                                         0x0300},
};

// CTS, DSR, RI and DCD in bits 4 to 7. Bits 0 and 1 report DTR and RTS, which are outputs
static constexpr modem_status MODEM_STATUSES[] = {
    {0x53, terminal::TIOCM_CTS | terminal::TIOCM_RNG},
    {0xA0, terminal::TIOCM_DSR | terminal::TIOCM_CAR},
    {0x03, 0},
};

// DTR raised alone sets SET_FLOW's first word, and RTS raised alone its second
static constexpr flow_words FLOW_WORDS[] = {
    {terminal::TIOCM_DTR, 0x01, 0x00},
    {terminal::TIOCM_RTS, 0x00, 0x40},
};

// Static, so calls that never end still find them after their test, which then leaves the translator unfreed
static fake_chip     g_chip;
static cp210x*       g_translator;
static line_settings g_settings;
static uint32_t      g_lines;
static bool          g_is_on;
static int32_t       g_rc;
static ssize_t       g_pending;

static bool attach_translator() {
    if (!attach_fake_chip(g_chip)) {
        return false;
    }

    usb::device& dev = g_chip.controller->usb_device;
    dev.num_interfaces = 2;
    for (uint8_t i = 0; i < dev.num_interfaces; i++) {
        dev.interfaces[i].interface_number = i;
    }

    g_translator = heap::ualloc_new<cp210x>(&dev, dev.interfaces[INTERFACE_NUMBER]);
    if (!g_translator) {
        detach_fake_chip(g_chip);
        return false;
    }

    return true;
}

static void detach_translator() {
    if (!g_chip.is_call_stuck) {
        heap::ufree_delete(g_translator);
    }

    detach_fake_chip(g_chip);
}

static void configure() {
    g_rc = g_translator->configure();
}

static void set_line() {
    g_rc = g_translator->set_line(g_settings);
}

static void set_modem_outputs() {
    g_rc = g_translator->set_modem_outputs(g_lines);
}

static void get_modem_inputs() {
    g_rc = g_translator->get_modem_inputs(&g_lines);
}

static void set_break() {
    g_rc = g_translator->set_break(g_is_on);
}

static void tx_pending() {
    g_pending = g_translator->tx_pending();
}

static line_settings settings_of(uint32_t baud, uint8_t data_bits, parity mode, uint8_t stop_bits) {
    return {baud, data_bits, mode, stop_bits, false};
}

// Configures the translator as a chip whose part number request answers `number`
static bool configure_as(uint8_t number) {
    chip_request requests[CONFIGURE_REQUESTS];
    return make_calls(g_chip, configure, requests, CONFIGURE_REQUESTS, &number, sizeof(number)) && g_rc == serial::OK;
}

static uint32_t word_at(const chip_request& request, size_t offset) {
    return static_cast<uint32_t>(request.data[offset]) |
           static_cast<uint32_t>(request.data[offset + 1]) << 8 |
           static_cast<uint32_t>(request.data[offset + 2]) << 16 |
           static_cast<uint32_t>(request.data[offset + 3]) << 24;
}

static void expect_request(const chip_request& request, uint8_t type, uint8_t code, uint16_t value, uint16_t length) {
    EXPECT_EQ(request.request_type, type);
    EXPECT_EQ(request.request, code);
    EXPECT_EQ(request.value, value);
    EXPECT_EQ(request.index, static_cast<uint16_t>(INTERFACE_NUMBER));
    EXPECT_EQ(request.length, length);
}

// Sets `settings`, expecting its three requests, and gives the rate SET_BAUDRATE asked for
static uint32_t asked_rate(const line_settings& settings) {
    chip_request requests[SET_LINE_REQUESTS];
    g_settings = settings;
    EXPECT_TRUE(make_calls(g_chip, set_line, requests, SET_LINE_REQUESTS));
    EXPECT_EQ(g_rc, serial::OK);
    expect_request(requests[0], VENDOR_TO_INTERFACE, SET_BAUDRATE, 0, RATE_LENGTH);

    return word_at(requests[0], 0);
}

// True when the translator refuses `settings` without a request
static bool refuses(const line_settings& settings) {
    g_settings = settings;
    bool is_done = make_calls(g_chip, set_line, nullptr, 0);

    return is_done && g_rc == serial::ERR_UNSUPPORTED && sent_nothing(g_chip);
}

// The part number names the chip, and the UART is enabled last, after every other setup request
TEST(cp210x, configure_reads_the_part_number_then_enables_the_uart) {
    ASSERT_TRUE(attach_translator());

    chip_request requests[CONFIGURE_REQUESTS];
    uint8_t part = CP2102;
    EXPECT_TRUE(make_calls(g_chip, configure, requests, CONFIGURE_REQUESTS, &part, sizeof(part)));
    EXPECT_EQ(g_rc, serial::OK);

    expect_request(requests[0], VENDOR_FROM_INTERFACE, VENDOR_SPECIFIC, GET_PART_NUMBER, 1);
    expect_request(requests[1], VENDOR_TO_INTERFACE, IFC_ENABLE, 1, 0);
    EXPECT_TRUE(sent_nothing(g_chip));
    EXPECT_STREQ(g_translator->chip_name(), "cp2102");
    EXPECT_STREQ(g_translator->driver_name(), "cp210x");
    EXPECT_EQ(g_translator->channel(), -1);

    detach_translator();
}

TEST(cp210x, each_cp2102n_part_number_names_a_cp2102n) {
    ASSERT_TRUE(attach_translator());

    for (uint8_t part = CP2102N; part <= CP2102N + 2; part++) {
        EXPECT_TRUE(configure_as(part));
        EXPECT_STREQ(g_translator->chip_name(), "cp2102n");
    }

    detach_translator();
}

// A part the table lacks is given a CP2102's listed rates, so it is asked for the nearest one
TEST(cp210x, a_part_the_table_lacks_takes_the_family_name_and_a_cp2102s_rates) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(UNLISTED_PART));

    EXPECT_STREQ(g_translator->chip_name(), "cp210x");
    EXPECT_EQ(asked_rate(settings_of(9800, 8, parity::none, 1)), 9600u);

    detach_translator();
}

TEST(cp210x, a_chip_that_refuses_the_part_number_request_takes_the_family_name) {
    ASSERT_TRUE(attach_translator());

    sched::task* calls = start_calls(g_chip, configure);
    ASSERT_NOT_NULL(calls);
    chip_request part_number = {};
    chip_request enable = {};
    bool is_answered = refuse_request(g_chip, part_number) && answer_request(g_chip, enable, nullptr, 0);

    EXPECT_TRUE(finish_calls(g_chip, calls));
    EXPECT_TRUE(is_answered);
    EXPECT_EQ(g_rc, serial::OK);
    expect_request(enable, VENDOR_TO_INTERFACE, IFC_ENABLE, 1, 0);
    EXPECT_STREQ(g_translator->chip_name(), "cp210x");

    detach_translator();
}

TEST(cp210x, configure_after_an_unplug_fails_with_an_io_error) {
    ASSERT_TRUE(attach_translator());
    RUN_ELEVATED(g_chip.controller->device->set_ctrl_gone());

    EXPECT_TRUE(make_calls(g_chip, configure, nullptr, 0));
    EXPECT_EQ(g_rc, serial::ERR_IO);

    detach_translator();
}

// The rate is a little-endian word, and SET_FLOW leaves DTR and RTS lowered while the port has raised neither
TEST(cp210x, set_line_sends_the_rate_then_the_format_then_flow_control) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    chip_request requests[SET_LINE_REQUESTS];
    g_settings = settings_of(115200, 8, parity::none, 1);
    EXPECT_TRUE(make_calls(g_chip, set_line, requests, SET_LINE_REQUESTS));
    EXPECT_EQ(g_rc, serial::OK);
    expect_request(requests[0], VENDOR_TO_INTERFACE, SET_BAUDRATE, 0, RATE_LENGTH);
    EXPECT_EQ(word_at(requests[0], 0), 115200u);

    expect_request(requests[1], VENDOR_TO_INTERFACE, SET_LINE_CTL, 0x0800, 0);
    expect_request(requests[2], VENDOR_TO_INTERFACE, SET_FLOW, 0, FLOW_LENGTH);
    for (size_t offset = 0; offset < FLOW_LENGTH; offset += sizeof(uint32_t)) {
        EXPECT_EQ(word_at(requests[2], offset), 0u);
    }

    detach_translator();
}

TEST(cp210x, set_line_ctl_encodes_each_character_format) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    for (const character_format& format : CHARACTER_FORMATS) {
        chip_request requests[SET_LINE_REQUESTS];
        g_settings = settings_of(9600, format.data_bits, format.mode, format.stop_bits);
        EXPECT_TRUE(make_calls(g_chip, set_line, requests, SET_LINE_REQUESTS));
        EXPECT_EQ(g_rc, serial::OK);
        expect_request(requests[1], VENDOR_TO_INTERFACE, SET_LINE_CTL, format.line_control, 0);
    }

    detach_translator();
}

// A CP2102 is asked for the nearest of its listed rates, which it makes as 24 MHz over a whole divider, so the rate
// made must be within 3 percent, and it makes 921600 only with 7 or 8 data bits
TEST(cp210x, a_cp2102_gets_its_nearest_listed_rate_within_3_percent) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    EXPECT_EQ(asked_rate(settings_of(250000, 8, parity::none, 1)), 250000u);
    EXPECT_EQ(asked_rate(settings_of(9800, 8, parity::none, 1)), 9600u);
    EXPECT_EQ(asked_rate(settings_of(576000, 6, parity::none, 1)), 576000u);
    EXPECT_TRUE(refuses(settings_of(921600, 6, parity::none, 1)));
    EXPECT_TRUE(refuses(settings_of(110, 8, parity::none, 1)));

    // 921600 is made as 24 MHz over 26, 923077, which is within 3 percent of a rate up to 951626
    EXPECT_EQ(asked_rate(settings_of(951600, 8, parity::none, 1)), 921600u);
    EXPECT_TRUE(refuses(settings_of(951700, 8, parity::none, 1)));

    detach_translator();
}

// A CP2102N is asked for the rate itself, within its range, and makes 24 MHz over the nearest whole divider, so a rate
// passes when that division is within 3 percent of it
TEST(cp210x, a_cp2102n_takes_a_rate_whose_nearest_division_is_within_3_percent) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102N));

    EXPECT_EQ(asked_rate(settings_of(3000000, 5, parity::none, 1)), 3000000u);
    EXPECT_EQ(asked_rate(settings_of(3050000, 8, parity::none, 1)), 3000000u);
    EXPECT_EQ(asked_rate(settings_of(300, 8, parity::none, 1)), 300u);

    // 24 MHz over this rate is 20.7: dividing by 21 makes 1.4 percent less, by 20 it would make 3.5 percent more
    EXPECT_EQ(asked_rate(settings_of(1159420, 8, parity::none, 1)), 1159420u);
    EXPECT_TRUE(refuses(settings_of(2900000, 8, parity::none, 1)));
    EXPECT_TRUE(refuses(settings_of(200, 8, parity::none, 1)));

    detach_translator();
}

TEST(cp210x, a_cp2104_makes_5_or_6_data_bits_only_up_to_921600) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2104));

    EXPECT_EQ(asked_rate(settings_of(921600, 6, parity::none, 1)), 921600u);
    EXPECT_EQ(asked_rate(settings_of(2000000, 8, parity::none, 1)), 2000000u);
    EXPECT_TRUE(refuses(settings_of(1000000, 5, parity::none, 1)));

    detach_translator();
}

TEST(cp210x, set_mhs_sets_dtr_and_rts_in_one_request) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    for (const modem_outputs& outputs : MODEM_OUTPUTS) {
        chip_request request = {};
        g_lines = outputs.lines;
        EXPECT_TRUE(make_calls(g_chip, set_modem_outputs, &request, 1));
        EXPECT_EQ(g_rc, serial::OK);
        expect_request(request, VENDOR_TO_INTERFACE, SET_MHS, outputs.value, 0);
    }

    detach_translator();
}

// SET_FLOW also sets DTR and RTS, so it sets the raised lines again, and under flow control by CTS the chip drives RTS,
// which SET_MHS then leaves alone until flow control ends
TEST(cp210x, flow_control_keeps_the_lines_the_port_raised) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    chip_request raise = {};
    g_lines = terminal::TIOCM_DTR | terminal::TIOCM_RTS;
    EXPECT_TRUE(make_calls(g_chip, set_modem_outputs, &raise, 1));

    chip_request with_flow_control[SET_LINE_REQUESTS];
    g_settings = {115200, 8, parity::none, 1, true};
    EXPECT_TRUE(make_calls(g_chip, set_line, with_flow_control, SET_LINE_REQUESTS));
    EXPECT_EQ(word_at(with_flow_control[2], 0), 0x09u);
    EXPECT_EQ(word_at(with_flow_control[2], 4), 0x80u);

    chip_request raise_again = {};
    EXPECT_TRUE(make_calls(g_chip, set_modem_outputs, &raise_again, 1));
    EXPECT_EQ(raise_again.value, static_cast<uint16_t>(0x0101));

    chip_request without_flow_control[SET_LINE_REQUESTS];
    g_settings = settings_of(115200, 8, parity::none, 1);
    EXPECT_TRUE(make_calls(g_chip, set_line, without_flow_control, SET_LINE_REQUESTS));
    EXPECT_EQ(word_at(without_flow_control[2], 0), 0x01u);
    EXPECT_EQ(word_at(without_flow_control[2], 4), 0x40u);

    detach_translator();
}

// SET_FLOW sets DTR and RTS in separate words, so a speed change keeps either line raised alone
TEST(cp210x, flow_control_keeps_dtr_or_rts_raised_alone) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    for (const flow_words& words : FLOW_WORDS) {
        chip_request raise = {};
        g_lines = words.lines;
        EXPECT_TRUE(make_calls(g_chip, set_modem_outputs, &raise, 1));

        chip_request requests[SET_LINE_REQUESTS];
        g_settings = settings_of(115200, 8, parity::none, 1);
        EXPECT_TRUE(make_calls(g_chip, set_line, requests, SET_LINE_REQUESTS));
        EXPECT_EQ(word_at(requests[2], 0), words.handshake);
        EXPECT_EQ(word_at(requests[2], 4), words.rts_mode);
    }

    detach_translator();
}

TEST(cp210x, get_mdmsts_gives_each_input_line) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    for (const modem_status& status : MODEM_STATUSES) {
        chip_request request = {};
        EXPECT_TRUE(make_calls(g_chip, get_modem_inputs, &request, 1, &status.status, 1));
        EXPECT_EQ(g_rc, serial::OK);
        expect_request(request, VENDOR_FROM_INTERFACE, GET_MDMSTS, 0, 1);
        EXPECT_EQ(g_lines, status.inputs);
    }

    detach_translator();
}

// The count of bytes waiting to be sent is the reply's fourth word, here between words of set bits
TEST(cp210x, tx_pending_reads_the_bytes_waiting_to_be_sent) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    uint8_t status[COMM_STATUS_LENGTH];
    string::memset(status, 0xFF, sizeof(status));
    status[BYTES_TO_SEND_OFFSET] = 0x2C;
    status[BYTES_TO_SEND_OFFSET + 1] = 0x01;
    status[BYTES_TO_SEND_OFFSET + 2] = 0;
    status[BYTES_TO_SEND_OFFSET + 3] = 0;

    chip_request request = {};
    EXPECT_TRUE(make_calls(g_chip, tx_pending, &request, 1, status, sizeof(status)));
    expect_request(request, VENDOR_FROM_INTERFACE, GET_COMM_STATUS, 0, COMM_STATUS_LENGTH);
    EXPECT_EQ(g_pending, static_cast<ssize_t>(300));

    detach_translator();
}

TEST(cp210x, set_break_turns_the_break_on_and_off) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));

    chip_request on = {};
    g_is_on = true;
    EXPECT_TRUE(make_calls(g_chip, set_break, &on, 1));
    EXPECT_EQ(g_rc, serial::OK);
    expect_request(on, VENDOR_TO_INTERFACE, SET_BREAK, 1, 0);

    chip_request off = {};
    g_is_on = false;
    EXPECT_TRUE(make_calls(g_chip, set_break, &off, 1));
    expect_request(off, VENDOR_TO_INTERFACE, SET_BREAK, 0, 0);

    detach_translator();
}

// Once the device has left, every call fails at once with nothing sent
TEST(cp210x, calls_after_an_unplug_fail_with_an_io_error) {
    ASSERT_TRUE(attach_translator());
    ASSERT_TRUE(configure_as(CP2102));
    RUN_ELEVATED(g_chip.controller->device->set_ctrl_gone());

    g_settings = settings_of(115200, 8, parity::none, 1);
    EXPECT_TRUE(make_calls(g_chip, set_line, nullptr, 0));
    EXPECT_EQ(g_rc, serial::ERR_IO);

    g_lines = terminal::TIOCM_DTR;
    EXPECT_TRUE(make_calls(g_chip, set_modem_outputs, nullptr, 0));
    EXPECT_EQ(g_rc, serial::ERR_IO);
    EXPECT_TRUE(make_calls(g_chip, get_modem_inputs, nullptr, 0));
    EXPECT_EQ(g_rc, serial::ERR_IO);

    g_is_on = true;
    EXPECT_TRUE(make_calls(g_chip, set_break, nullptr, 0));
    EXPECT_EQ(g_rc, serial::ERR_IO);
    EXPECT_TRUE(make_calls(g_chip, tx_pending, nullptr, 0));
    EXPECT_EQ(g_pending, static_cast<ssize_t>(serial::ERR_IO));

    detach_translator();
}
