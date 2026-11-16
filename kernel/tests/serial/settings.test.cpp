#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/serial/settings.h"
#include "common/string.h"

TEST_SUITE(serial_settings);

using serial::default_termios;
using serial::line_settings;
using serial::parity;
using serial::translate_termios;
using terminal::termios2;

struct speed_code {
    uint32_t code;
    uint32_t rate;
};

// Every speed code but BOTHER, with the rate it names
static constexpr speed_code SPEED_CODES[] = {
    {0x0000, 0},       {0x0001, 50},      {0x0002, 75},      {0x0003, 110},
    {0x0004, 134},     {0x0005, 150},     {0x0006, 200},     {0x0007, 300},
    {0x0008, 600},     {0x0009, 1200},    {0x000A, 1800},    {0x000B, 2400},
    {0x000C, 4800},    {0x000D, 9600},    {0x000E, 19200},   {0x000F, 38400},
    {0x1001, 57600},   {0x1002, 115200},  {0x1003, 230400},  {0x1004, 460800},
    {0x1005, 500000},  {0x1006, 576000},  {0x1007, 921600},  {0x1008, 1000000},
    {0x1009, 1152000}, {0x100A, 1500000}, {0x100B, 2000000}, {0x100C, 2500000},
    {0x100D, 3000000}, {0x100E, 3500000}, {0x100F, 4000000},
};

static constexpr uint32_t B115200 = 0x1002;

// A rate no speed code names
static constexpr uint32_t NONSTANDARD_RATE = 250000;

// Settings no translation of a new port's settings gives, so a refusal that writes any field shows
static constexpr line_settings UNTOUCHED_LINE = {
    .baud = NONSTANDARD_RATE,
    .data_bits = 7,
    .parity_mode = parity::mark,
    .stop_bits = 2,
    .uses_rts_cts = true,
};

struct refused_flag {
    uint32_t termios2::* word;
    uint32_t flag;
};

static constexpr refused_flag REFUSED_FLAGS[] = {
    {&termios2::c_iflag, terminal::PARMRK},
    {&termios2::c_iflag, terminal::ISTRIP},
    {&termios2::c_iflag, terminal::INLCR},
    {&termios2::c_iflag, terminal::IGNCR},
    {&termios2::c_iflag, terminal::ICRNL},
    {&termios2::c_iflag, terminal::IUCLC},
    {&termios2::c_iflag, terminal::IXON},
    {&termios2::c_iflag, terminal::IXOFF},
    {&termios2::c_oflag, terminal::OPOST},
    {&termios2::c_lflag, terminal::ISIG},
    {&termios2::c_lflag, terminal::ICANON},
    {&termios2::c_lflag, terminal::ECHO},
    {&termios2::c_lflag, terminal::ECHONL},
};

// The c_cflag bits `translate_termios` does not read
static constexpr uint32_t UNTRANSLATED_CFLAGS = ~(terminal::CBAUD | terminal::CIBAUD | terminal::CSIZE |
                                                  terminal::CSTOPB | terminal::PARENB | terminal::PARODD |
                                                  terminal::CMSPAR | terminal::CRTSCTS);

static termios2 settings_with_speed(uint32_t code) {
    termios2 settings = default_termios();
    settings.c_cflag = (settings.c_cflag & ~terminal::CBAUD) | code;
    return settings;
}

static termios2 settings_with_cflags(uint32_t flags) {
    termios2 settings = default_termios();
    settings.c_cflag |= flags;
    return settings;
}

static bool line_settings_equal(const line_settings& a, const line_settings& b) {
    return a.baud == b.baud &&
           a.data_bits == b.data_bits &&
           a.parity_mode == b.parity_mode &&
           a.stop_bits == b.stop_bits &&
           a.uses_rts_cts == b.uses_rts_cts;
}

// True when `settings` are refused and the line settings they were to fill are left as they were
static bool is_refused_untouched(const termios2& settings) {
    line_settings line = UNTOUCHED_LINE;
    int32_t rc = translate_termios(settings, &line);

    return rc == serial::ERR_UNSUPPORTED && line_settings_equal(line, UNTOUCHED_LINE);
}

TEST(serial_settings, a_new_port_starts_raw_with_reads_that_wait_for_one_byte) {
    termios2 expected = {};
    expected.c_cflag = terminal::B9600 | terminal::CS8 | terminal::CREAD | terminal::CLOCAL | terminal::HUPCL;
    expected.c_cc[terminal::VMIN] = 1;
    expected.c_ispeed = 9600;
    expected.c_ospeed = 9600;

    termios2 settings = default_termios();
    EXPECT_EQ(string::memcmp(&settings, &expected, sizeof(expected)), 0);
}

TEST(serial_settings, a_new_port_asks_for_9600_8n1_without_flow_control) {
    line_settings line = {};
    ASSERT_EQ(translate_termios(default_termios(), &line), serial::OK);

    EXPECT_EQ(line.baud, 9600u);
    EXPECT_EQ(line.data_bits, 8u);
    EXPECT_EQ(line.parity_mode, parity::none);
    EXPECT_EQ(line.stop_bits, 1u);
    EXPECT_FALSE(line.uses_rts_cts);
}

TEST(serial_settings, every_speed_code_asks_for_the_rate_it_names) {
    for (const speed_code& entry : SPEED_CODES) {
        line_settings line = {};
        ASSERT_EQ(translate_termios(settings_with_speed(entry.code), &line), serial::OK);
        EXPECT_EQ(line.baud, entry.rate);
    }
}

TEST(serial_settings, bother_asks_for_the_output_speed_as_written) {
    termios2 settings = settings_with_speed(terminal::BOTHER);
    settings.c_ospeed = NONSTANDARD_RATE;

    line_settings line = {};
    ASSERT_EQ(translate_termios(settings, &line), serial::OK);
    EXPECT_EQ(line.baud, NONSTANDARD_RATE);
}

TEST(serial_settings, bother_with_an_output_speed_of_0_is_refused) {
    termios2 settings = settings_with_speed(terminal::BOTHER);
    settings.c_ospeed = 0;

    EXPECT_TRUE(is_refused_untouched(settings));
}

TEST(serial_settings, an_input_speed_of_0_or_the_output_speed_is_accepted) {
    termios2 settings = settings_with_speed(B115200);
    line_settings line = {};

    settings.c_cflag |= B115200 << terminal::IBSHIFT;
    EXPECT_EQ(translate_termios(settings, &line), serial::OK);

    settings.c_cflag &= ~terminal::CIBAUD;
    settings.c_cflag |= terminal::BOTHER << terminal::IBSHIFT;
    settings.c_ispeed = 115200;
    EXPECT_EQ(translate_termios(settings, &line), serial::OK);

    settings.c_ispeed = 0;
    EXPECT_EQ(translate_termios(settings, &line), serial::OK);
    EXPECT_EQ(line.baud, 115200u);
}

TEST(serial_settings, split_speeds_are_refused) {
    termios2 settings = settings_with_speed(B115200);
    settings.c_cflag |= terminal::B9600 << terminal::IBSHIFT;
    EXPECT_TRUE(is_refused_untouched(settings));

    settings = settings_with_speed(terminal::BOTHER);
    settings.c_cflag |= terminal::BOTHER << terminal::IBSHIFT;
    settings.c_ospeed = 115200;
    settings.c_ispeed = 9600;
    EXPECT_TRUE(is_refused_untouched(settings));
}

TEST(serial_settings, csize_asks_for_5_to_8_data_bits) {
    struct data_bits_case {
        uint32_t csize;
        uint8_t  data_bits;
    };

    static constexpr data_bits_case CASES[] = {
        {terminal::CS5, 5}, {terminal::CS6, 6}, {terminal::CS7, 7}, {terminal::CS8, 8},
    };

    for (const data_bits_case& entry : CASES) {
        termios2 settings = default_termios();
        settings.c_cflag = (settings.c_cflag & ~terminal::CSIZE) | entry.csize;

        line_settings line = {};
        ASSERT_EQ(translate_termios(settings, &line), serial::OK);
        EXPECT_EQ(line.data_bits, entry.data_bits);
    }
}

TEST(serial_settings, parity_flags_pick_the_parity_mode) {
    struct parity_case {
        uint32_t flags;
        parity   parity_mode;
    };

    static constexpr parity_case CASES[] = {
        {terminal::PARODD | terminal::CMSPAR, parity::none},
        {terminal::PARENB, parity::even},
        {terminal::PARENB | terminal::PARODD, parity::odd},
        {terminal::PARENB | terminal::CMSPAR, parity::space},
        {terminal::PARENB | terminal::PARODD | terminal::CMSPAR, parity::mark},
    };

    for (const parity_case& entry : CASES) {
        line_settings line = {};
        ASSERT_EQ(translate_termios(settings_with_cflags(entry.flags), &line), serial::OK);
        EXPECT_EQ(line.parity_mode, entry.parity_mode);
    }
}

TEST(serial_settings, cstopb_asks_for_2_stop_bits) {
    line_settings line = {};
    ASSERT_EQ(translate_termios(settings_with_cflags(terminal::CSTOPB), &line), serial::OK);
    EXPECT_EQ(line.stop_bits, 2u);
}

TEST(serial_settings, crtscts_asks_for_flow_control_by_cts) {
    line_settings line = {};
    ASSERT_EQ(translate_termios(settings_with_cflags(terminal::CRTSCTS), &line), serial::OK);
    EXPECT_TRUE(line.uses_rts_cts);
}

TEST(serial_settings, each_flag_that_would_edit_echo_drop_or_pause_is_refused) {
    for (const refused_flag& entry : REFUSED_FLAGS) {
        termios2 settings = default_termios();
        settings.*entry.word |= entry.flag;

        EXPECT_TRUE(is_refused_untouched(settings));
    }
}

TEST(serial_settings, every_other_flag_is_accepted_without_effect) {
    termios2 settings = default_termios();
    settings.c_iflag = ~0u;
    settings.c_oflag = ~0u;
    settings.c_lflag = ~0u;
    for (const refused_flag& entry : REFUSED_FLAGS) {
        settings.*entry.word &= ~entry.flag;
    }

    settings.c_cflag ^= UNTRANSLATED_CFLAGS;
    settings.c_cc[terminal::VMIN] = 0;
    settings.c_cc[terminal::VTIME] = 10;

    line_settings expected = {};
    line_settings line = {};
    ASSERT_EQ(translate_termios(default_termios(), &expected), serial::OK);
    ASSERT_EQ(translate_termios(settings, &line), serial::OK);
    EXPECT_TRUE(line_settings_equal(line, expected));
}
