#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "terminal/terminal.h"
#include "terminal/tty.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "common/string.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"

TEST_SUITE(console_requests);

using test_helpers::user_page;
using test_helpers::user_space_scope;

constexpr uint32_t COOKED_LFLAG = terminal::ICANON | terminal::ECHO | terminal::ISIG;

// /dev/console opened by the calling task, with a user page that carries each request's argument
struct console_file {
    user_page page;
    resource::handle_t handle = -1;

    console_file() {
        if (page.ready() && resource::open(sched::current(), "/dev/console", fs::O_RDWR, &handle) != resource::OK) {
            handle = -1;
        }
    }

    ~console_file() {
        if (handle >= 0) {
            (void)resource::close(sched::current(), handle);
        }
    }

    bool ready() const { return handle >= 0; }

    // The result a program gets from the request with its argument at `address`
    int64_t request(uint32_t cmd, uint64_t address) {
        user_space_scope scope(page.ctx);
        return sys_ioctl(static_cast<uint64_t>(handle), cmd, address, 0, 0, 0);
    }

    // The same, with the argument at the start of the page
    int64_t request(uint32_t cmd) { return request(cmd, page.addr); }
};

// Hands `keys` to the console as its UART's receive interrupt does
static void type_on_console(const char* keys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        terminal::input_char(nullptr, static_cast<uint8_t>(keys[i]));
    }
}

// Puts the console back in its boot state, cooked with ISIG, 0 by 0 and no input, even after an assertion ended a
// test early
static int32_t restore_console() {
    (void)terminal::set_mode(terminal::STLX_TCSETS_COOKED);

    console_file console;
    if (console.ready()) {
        *console.page.at<terminal::winsize>(0) = {};
        (void)console.request(terminal::TIOCSWINSZ);
    }

    uint8_t input[64];
    ssize_t got = 0;
    do {
        got = terminal::console_read(input, sizeof(input), true);
    } while (got >= 0);

    return 0;
}

AFTER_EACH(console_requests, restore_console);

TEST(console_requests, window_size_reads_0_by_0_until_a_program_sets_one) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* size = console.page.at<terminal::winsize>(0);
    *size = {1, 1, 1, 1};
    EXPECT_EQ(console.request(terminal::TIOCGWINSZ), 0);

    EXPECT_EQ(size->ws_row, 0);
    EXPECT_EQ(size->ws_col, 0);
    EXPECT_EQ(size->ws_xpixel, 0);
    EXPECT_EQ(size->ws_ypixel, 0);

    *size = {50, 132, 1056, 800};
    EXPECT_EQ(console.request(terminal::TIOCSWINSZ), 0);

    *size = {};
    EXPECT_EQ(console.request(terminal::TIOCGWINSZ), 0);

    EXPECT_EQ(size->ws_row, 50);
    EXPECT_EQ(size->ws_col, 132);
    EXPECT_EQ(size->ws_xpixel, 1056);
    EXPECT_EQ(size->ws_ypixel, 800);
}

TEST(console_requests, termios_reads_icanon_echo_and_isig_in_cooked_mode_and_0_elsewhere) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* settings = console.page.at<terminal::termios>(0);
    string::memset(settings, 0xA5, sizeof(*settings));
    ASSERT_EQ(console.request(terminal::TCGETS), 0);

    terminal::termios expected = {};
    expected.c_lflag = COOKED_LFLAG;
    EXPECT_EQ(string::memcmp(settings, &expected, sizeof(expected)), 0);
}

// A program going raw by clearing ICANON, ECHO and ISIG with TCSETSF, then putting back what it saved
TEST(console_requests, termios_without_icanon_or_isig_passes_control_bytes_and_saved_termios_restores_cooked) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* settings = console.page.at<terminal::termios>(0);
    ASSERT_EQ(console.request(terminal::TCGETS), 0);
    terminal::termios saved = *settings;

    settings->c_lflag &= ~COOKED_LFLAG;
    ASSERT_EQ(console.request(terminal::TCSETSF), 0);

    // Ctrl C and Ctrl ], which a serial monitor quits on, arrive as bytes
    type_on_console("a\x03\x1d", 3);
    uint8_t got[8] = {};
    EXPECT_EQ(terminal::console_read(got, sizeof(got), true), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(got, "a\x03\x1d", 3), 0);

    *settings = saved;
    EXPECT_EQ(console.request(terminal::TCSETSW), 0);

    *settings = {};
    EXPECT_EQ(console.request(terminal::TCGETS), 0);
    EXPECT_EQ(settings->c_lflag, COOKED_LFLAG);
}

TEST(console_requests, termios_with_isig_but_not_icanon_takes_ctrl_c_out_of_raw_input) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* settings = console.page.at<terminal::termios>(0);
    *settings = {};
    settings->c_lflag = terminal::ISIG;
    ASSERT_EQ(console.request(terminal::TCSETS), 0);

    *settings = {};
    EXPECT_EQ(console.request(terminal::TCGETS), 0);
    EXPECT_EQ(settings->c_lflag, terminal::ISIG);

    type_on_console("x\x03y", 3);
    uint8_t got[8] = {};
    EXPECT_EQ(terminal::console_read(got, sizeof(got), true), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(got, "xy", 2), 0);
}

// A password prompt clears ECHO and keeps ICANON, so its line arrives whole and with no echo
TEST(console_requests, termios_without_echo_keeps_cooked_lines_and_reads_back_without_echo) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* settings = console.page.at<terminal::termios>(0);
    *settings = {};
    settings->c_lflag = terminal::ICANON | terminal::ISIG;
    ASSERT_EQ(console.request(terminal::TCSETS), 0);

    *settings = {};
    EXPECT_EQ(console.request(terminal::TCGETS), 0);
    EXPECT_EQ(settings->c_lflag, terminal::ICANON | terminal::ISIG);

    type_on_console("pw\r", 3);
    uint8_t got[8] = {};
    EXPECT_EQ(terminal::console_read(got, sizeof(got), true), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(got, "pw\n", 3), 0);
}

// The shell switches its standard input with these around every command it runs, so they also turn echo back on
TEST(console_requests, stellux_mode_requests_switch_the_mode_tcgets_reports_and_turn_echo_back_on) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* settings = console.page.at<terminal::termios>(0);
    *settings = {};
    settings->c_lflag = terminal::ICANON | terminal::ISIG;
    ASSERT_EQ(console.request(terminal::TCSETS), 0);

    ASSERT_EQ(console.request(terminal::STLX_TCSETS_RAW, 0), 0);
    ASSERT_EQ(console.request(terminal::TCGETS), 0);
    EXPECT_EQ(settings->c_lflag, 0u);

    ASSERT_EQ(console.request(terminal::STLX_TCSETS_COOKED, 0), 0);
    ASSERT_EQ(console.request(terminal::TCGETS), 0);
    EXPECT_EQ(settings->c_lflag, COOKED_LFLAG);
}

constexpr uint64_t UNMAPPED_ADDRESS = 0;

TEST(console_requests, requests_with_an_unreadable_argument_fail_with_einval_and_change_nothing) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* size = console.page.at<terminal::winsize>(0);
    *size = {50, 132, 0, 0};
    ASSERT_EQ(console.request(terminal::TIOCSWINSZ), 0);

    EXPECT_EQ(console.request(terminal::TCGETS, UNMAPPED_ADDRESS), syscall::EINVAL);
    EXPECT_EQ(console.request(terminal::TCSETS, UNMAPPED_ADDRESS), syscall::EINVAL);
    EXPECT_EQ(console.request(terminal::TIOCGWINSZ, UNMAPPED_ADDRESS), syscall::EINVAL);
    EXPECT_EQ(console.request(terminal::TIOCSWINSZ, UNMAPPED_ADDRESS), syscall::EINVAL);

    *size = {};
    EXPECT_EQ(console.request(terminal::TIOCGWINSZ), 0);
    EXPECT_EQ(size->ws_row, 50);
    EXPECT_EQ(size->ws_col, 132);

    auto* settings = console.page.at<terminal::termios>(0);
    EXPECT_EQ(console.request(terminal::TCGETS), 0);
    EXPECT_EQ(settings->c_lflag, COOKED_LFLAG);
}

constexpr int32_t MISSING_GROUP = 0x7FFFFFFF;

TEST(console_requests, foreground_group_reads_0_and_refuses_a_group_that_does_not_exist) {
    console_file console;
    ASSERT_TRUE(console.ready());

    auto* group = console.page.at<int32_t>(0);
    *group = -1;
    EXPECT_EQ(console.request(terminal::TIOCGPGRP), 0);
    EXPECT_EQ(*group, 0);

    *group = MISSING_GROUP;
    EXPECT_EQ(console.request(terminal::TIOCSPGRP), syscall::EINVAL);

    *group = -1;
    EXPECT_EQ(console.request(terminal::TIOCGPGRP), 0);
    EXPECT_EQ(*group, 0);
}

constexpr uint32_t UNKNOWN_REQUEST = 0x54FFu;

TEST(console_requests, unknown_request_fails_with_enotty) {
    console_file console;
    ASSERT_TRUE(console.ready());

    EXPECT_EQ(console.request(UNKNOWN_REQUEST), syscall::ENOTTY);
}
