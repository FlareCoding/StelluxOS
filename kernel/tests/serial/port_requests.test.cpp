#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/serial/serial.h"
#include "drivers/serial/settings.h"
#include "tests/serial/fake_port_driver.h"
#include "terminal/tty.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "common/string.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/atomic.h"
#include "sync/poll.h"

TEST_SUITE(serial_port_requests);

using serial::parity;
using serial_test::fake_port_driver;
using test_helpers::blocks_before_deadline;
using test_helpers::spin_wait;
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

// A board no other test names, and its port's path
static constexpr serial::identity BOARD = {
    .chip = "reqchip",
    .serial_number = "SN7777",
    .location = "8.1",
    .channel = -1,
};

static constexpr const char* PORT_PATH = "/dev/serial/reqchip-7777";

static constexpr uint32_t DEFAULT_CFLAG_WITHOUT_SPEED = terminal::CS8 | terminal::CREAD | terminal::CLOCAL |
                                                        terminal::HUPCL;

// A port file opened by the calling task, with a user page that carries each request's argument
struct port_file {
    user_page          page;
    resource::handle_t handle = -1;
    int32_t            open_result = resource::OK;

    port_file() {
        open_result = page.ready()
            ? resource::open(sched::current(), PORT_PATH, fs::O_RDWR, &handle)
            : resource::ERR_NOMEM;
    }

    ~port_file() {
        if (open_result == resource::OK) {
            (void)resource::close(sched::current(), handle);
        }
    }

    bool ready() const { return open_result == resource::OK; }

    // The result a program gets from the request with its argument at the start of the page
    int64_t request(uint32_t cmd) {
        user_space_scope scope(page.ctx);
        return sys_ioctl(static_cast<uint64_t>(handle), cmd, page.addr, 0, 0, 0);
    }

    // The same with an argument that is a number rather than an address
    int64_t request_with_value(uint32_t cmd, uint64_t value) {
        user_space_scope scope(page.ctx);
        return sys_ioctl(static_cast<uint64_t>(handle), cmd, value, 0, 0, 0);
    }
};

// A call a helper task makes on the port, since a chip request the test holds keeps it waiting
struct port_call {
    fs::file*              file;
    user_page*             page;
    uint32_t               cmd;
    int32_t                result;
    sync::atomic<uint32_t> is_done;
};

static fake_port_driver g_driver;
static serial::port*    g_port;
static bool             g_is_detached;
static port_call        g_call;
static port_call        g_unregister_call;

static int32_t register_board() {
    g_driver.reset();
    g_port = serial::register_port(&g_driver, BOARD);
    g_is_detached = false;
    return g_port ? 0 : -1;
}

// What the board's driver does when its device leaves
static void detach_board() {
    serial::detach(g_port);
    g_is_detached = true;
}

static int32_t unregister_board() {
    if (!g_port) {
        return 0;
    }

    if (!g_is_detached) {
        detach_board();
    }

    serial::unregister_port(g_port);
    g_port = nullptr;
    return 0;
}

BEFORE_EACH(serial_port_requests, register_board);
AFTER_EACH(serial_port_requests, unregister_board);

static void receive_text(const char* text) {
    serial::receive(g_port, reinterpret_cast<const uint8_t*>(text), string::strlen(text));
}

static void finish_call_task(port_call& call) {
    call.is_done.store_release(1);
    sched::exit(0);
}

// A request whose argument sits at the start of the call's page, as a program's syscall makes it. The page's address
// space must be left before the task exits
static void run_request(void*) {
    {
        user_space_scope scope(g_call.page->ctx);
        g_call.result = fs::ioctl(g_call.file, g_call.cmd, g_call.page->addr);
    }

    finish_call_task(g_call);
}

static void run_open(void*) {
    g_call.file = fs::open(PORT_PATH, fs::O_RDWR);
    finish_call_task(g_call);
}

static void run_unregister(void*) {
    serial::unregister_port(g_port);
    finish_call_task(g_unregister_call);
}

// Starts `entry` on an elevated task, as a syscall runs
static sched::task* start_call(port_call& call, void (*entry)(void*)) {
    call.is_done.store_release(0);
    return start_pinned_task_on_another_cpu(entry, nullptr, "serial_port_call", sched::TASK_FLAG_ELEVATED);
}

static bool finish_call(port_call& call, sched::task* task) {
    bool is_done = spin_wait(call.is_done);
    unpin(task);
    return is_done;
}

// What the board's driver does when it unregisters, after which the test's AFTER_EACH has nothing left to do
static void forget_board() {
    g_port = nullptr;
}

static bool lines_equal(const serial::line_settings& a, const serial::line_settings& b) {
    return a.baud == b.baud &&
           a.data_bits == b.data_bits &&
           a.parity_mode == b.parity_mode &&
           a.stop_bits == b.stop_bits &&
           a.uses_rts_cts == b.uses_rts_cts;
}

static terminal::termios with_cflag(const terminal::termios& settings, uint32_t cflag) {
    terminal::termios changed = settings;
    changed.c_cflag = cflag;
    return changed;
}

TEST(serial_port_requests, the_first_open_sets_the_chip_to_the_port_settings) {
    EXPECT_EQ(g_driver.set_line_calls, 0u);

    port_file first;
    ASSERT_TRUE(first.ready());
    port_file second;
    ASSERT_TRUE(second.ready());

    EXPECT_EQ(g_driver.set_line_calls, 1u);
    EXPECT_TRUE(lines_equal(g_driver.line, {9600, 8, parity::none, 1, false}));
}

TEST(serial_port_requests, a_new_port_reads_back_the_default_settings) {
    port_file file;
    ASSERT_TRUE(file.ready());

    ASSERT_EQ(file.request(terminal::TCGETS2), 0);
    terminal::termios2 expected = serial::default_termios();
    EXPECT_EQ(string::memcmp(file.page.at<terminal::termios2>(0), &expected, sizeof(expected)), 0);
}

// TCSETS writes the first 36 bytes of the stored termios2 and TCGETS reads them back exactly, while the speeds
// stored beyond them stay as they were
TEST(serial_port_requests, tcsets_sets_the_chip_and_reads_back_exactly) {
    port_file file;
    ASSERT_TRUE(file.ready());
    ASSERT_EQ(file.request(terminal::TCGETS), 0);

    uint32_t cflag = terminal::B115200 | terminal::CS7 | terminal::PARENB | terminal::CSTOPB | terminal::CREAD;
    terminal::termios written = with_cflag(*file.page.at<terminal::termios>(0), cflag);
    written.c_cc[terminal::VMIN] = 0;
    written.c_cc[terminal::VTIME] = 7;
    *file.page.at<terminal::termios>(0) = written;
    ASSERT_EQ(file.request(terminal::TCSETS), 0);

    EXPECT_TRUE(lines_equal(g_driver.line, {115200, 7, parity::even, 2, false}));

    string::memset(file.page.at<uint8_t>(0), 0xFF, sizeof(terminal::termios2));
    ASSERT_EQ(file.request(terminal::TCGETS2), 0);
    auto* read_back = file.page.at<terminal::termios2>(0);
    EXPECT_EQ(string::memcmp(read_back, &written, sizeof(written)), 0);
    EXPECT_EQ(read_back->c_ospeed, 9600u);
}

TEST(serial_port_requests, tcsets2_takes_a_speed_no_code_names) {
    port_file file;
    ASSERT_TRUE(file.ready());
    ASSERT_EQ(file.request(terminal::TCGETS2), 0);

    terminal::termios2 written = *file.page.at<terminal::termios2>(0);
    written.c_cflag = terminal::BOTHER | DEFAULT_CFLAG_WITHOUT_SPEED;
    written.c_ospeed = 250000;
    written.c_ispeed = 0;
    *file.page.at<terminal::termios2>(0) = written;
    ASSERT_EQ(file.request(terminal::TCSETS2), 0);

    EXPECT_EQ(g_driver.line.baud, 250000u);

    string::memset(file.page.at<uint8_t>(0), 0, sizeof(terminal::termios2));
    ASSERT_EQ(file.request(terminal::TCGETS2), 0);
    EXPECT_EQ(string::memcmp(file.page.at<terminal::termios2>(0), &written, sizeof(written)), 0);
}

// A flag the port refuses never reaches the chip, and a setting the chip refuses leaves the stored settings alone
TEST(serial_port_requests, a_refused_setting_fails_with_einval_and_changes_nothing) {
    port_file file;
    ASSERT_TRUE(file.ready());
    ASSERT_EQ(file.request(terminal::TCGETS), 0);
    terminal::termios before = *file.page.at<terminal::termios>(0);
    uint32_t calls_before = g_driver.set_line_calls;

    file.page.at<terminal::termios>(0)->c_iflag = terminal::ICRNL;
    EXPECT_EQ(file.request(terminal::TCSETS), syscall::EINVAL);
    EXPECT_EQ(g_driver.set_line_calls, calls_before);

    g_driver.set_line_result = serial::ERR_UNSUPPORTED;
    *file.page.at<terminal::termios>(0) = with_cflag(before, terminal::B115200 | DEFAULT_CFLAG_WITHOUT_SPEED);
    EXPECT_EQ(file.request(terminal::TCSETS), syscall::EINVAL);

    ASSERT_EQ(file.request(terminal::TCGETS), 0);
    EXPECT_EQ(string::memcmp(file.page.at<terminal::termios>(0), &before, sizeof(before)), 0);
}

TEST(serial_port_requests, a_failed_chip_request_fails_with_eio_and_changes_nothing) {
    port_file file;
    ASSERT_TRUE(file.ready());
    ASSERT_EQ(file.request(terminal::TCGETS), 0);
    terminal::termios before = *file.page.at<terminal::termios>(0);

    g_driver.set_line_result = serial::ERR_IO;
    *file.page.at<terminal::termios>(0) = with_cflag(before, terminal::B115200 | DEFAULT_CFLAG_WITHOUT_SPEED);
    EXPECT_EQ(file.request(terminal::TCSETSW), syscall::EIO);

    ASSERT_EQ(file.request(terminal::TCGETS), 0);
    EXPECT_EQ(string::memcmp(file.page.at<terminal::termios>(0), &before, sizeof(before)), 0);
}

// A chip is never asked for B0's speed of 0, so the rest of the settings go out at the speed it has
TEST(serial_port_requests, b0_keeps_the_chips_speed) {
    port_file file;
    ASSERT_TRUE(file.ready());
    ASSERT_EQ(file.request(terminal::TCGETS), 0);
    terminal::termios settings = *file.page.at<terminal::termios>(0);

    *file.page.at<terminal::termios>(0) = with_cflag(settings, terminal::B115200 | DEFAULT_CFLAG_WITHOUT_SPEED);
    ASSERT_EQ(file.request(terminal::TCSETS), 0);
    *file.page.at<terminal::termios>(0) = with_cflag(settings, terminal::CS7 | terminal::CREAD | terminal::CLOCAL);
    ASSERT_EQ(file.request(terminal::TCSETS), 0);

    EXPECT_TRUE(lines_equal(g_driver.line, {115200, 7, parity::none, 1, false}));
    ASSERT_EQ(file.request(terminal::TCGETS), 0);
    EXPECT_EQ(file.page.at<terminal::termios>(0)->c_cflag & terminal::CBAUD, 0u);
}

TEST(serial_port_requests, tcsetsf_empties_the_receive_queue) {
    port_file file;
    ASSERT_TRUE(file.ready());
    receive_text("stale");

    ASSERT_EQ(file.request(terminal::TCGETS), 0);
    ASSERT_EQ(file.request(terminal::TCSETSF), 0);

    ASSERT_EQ(file.request(terminal::FIONREAD), 0);
    EXPECT_EQ(*file.page.at<int32_t>(0), 0);
}

TEST(serial_port_requests, tcflsh_empties_the_receive_queue_and_refuses_an_unknown_queue) {
    port_file file;
    ASSERT_TRUE(file.ready());
    receive_text("stale");

    EXPECT_EQ(file.request_with_value(terminal::TCFLSH, terminal::TCIFLUSH), 0);
    ASSERT_EQ(file.request(terminal::FIONREAD), 0);
    EXPECT_EQ(*file.page.at<int32_t>(0), 0);

    EXPECT_EQ(file.request_with_value(terminal::TCFLSH, terminal::TCIOFLUSH + 1), syscall::EINVAL);
}

// A port answers TIOCGWINSZ, so isatty() is true for it, with a size that reads 0 by 0 until a program sets one
TEST(serial_port_requests, the_window_size_reads_back_what_was_set) {
    port_file file;
    ASSERT_TRUE(file.ready());

    *file.page.at<terminal::winsize>(0) = {1, 1, 1, 1};
    ASSERT_EQ(file.request(terminal::TIOCGWINSZ), 0);
    EXPECT_EQ(file.page.at<terminal::winsize>(0)->ws_row, 0);
    EXPECT_EQ(file.page.at<terminal::winsize>(0)->ws_col, 0);

    *file.page.at<terminal::winsize>(0) = {24, 80, 0, 0};
    ASSERT_EQ(file.request(terminal::TIOCSWINSZ), 0);
    *file.page.at<terminal::winsize>(0) = {};
    ASSERT_EQ(file.request(terminal::TIOCGWINSZ), 0);
    EXPECT_EQ(file.page.at<terminal::winsize>(0)->ws_row, 24);
    EXPECT_EQ(file.page.at<terminal::winsize>(0)->ws_col, 80);
}

TEST(serial_port_requests, exclusive_mode_refuses_other_opens_until_the_last_close) {
    {
        port_file holder;
        ASSERT_TRUE(holder.ready());
        ASSERT_EQ(holder.request(terminal::TIOCEXCL), 0);

        port_file refused;
        EXPECT_EQ(refused.open_result, resource::ERR_BUSY);

        ASSERT_EQ(holder.request(terminal::TIOCNXCL), 0);
        port_file allowed;
        EXPECT_TRUE(allowed.ready());

        ASSERT_EQ(holder.request(terminal::TIOCEXCL), 0);
    }

    port_file after_last_close;
    EXPECT_TRUE(after_last_close.ready());
}

// TCXONC suspends or restarts output, which a port refuses like XON and XOFF
TEST(serial_port_requests, tcxonc_fails_with_einval_and_unknown_requests_with_enotty) {
    port_file file;
    ASSERT_TRUE(file.ready());

    EXPECT_EQ(file.request_with_value(terminal::TCXONC, 0), syscall::EINVAL);
    EXPECT_EQ(file.request(terminal::TIOCGPGRP), syscall::ENOTTY);
}

// A failed chip request leaves the port closed, so bytes that arrive meanwhile are dropped and the next open sends the
// settings again
TEST(serial_port_requests, a_failed_first_open_leaves_the_port_closed) {
    g_driver.set_line_result = serial::ERR_IO;
    port_file failed;
    EXPECT_FALSE(failed.ready());
    const char text[] = "dropped";
    serial::receive(g_port, reinterpret_cast<const uint8_t*>(text), sizeof(text) - 1);

    g_driver.set_line_result = serial::OK;
    port_file file;
    ASSERT_TRUE(file.ready());
    EXPECT_EQ(g_driver.set_line_calls, 2u);
    ASSERT_EQ(file.request(terminal::FIONREAD), 0);
    EXPECT_EQ(*file.page.at<int32_t>(0), 0);
}

// TCSETSF2 empties the receive queue, and TCSETSW2 leaves it
TEST(serial_port_requests, the_termios2_forms_empty_or_keep_the_queue_as_their_termios_forms_do) {
    port_file file;
    ASSERT_TRUE(file.ready());
    receive_text("abc");
    ASSERT_EQ(file.request(terminal::TCGETS2), 0);

    ASSERT_EQ(file.request(terminal::TCSETSW2), 0);
    ASSERT_EQ(file.request(terminal::FIONREAD), 0);
    EXPECT_EQ(*file.page.at<int32_t>(0), 3);

    ASSERT_EQ(file.request(terminal::TCGETS2), 0);
    ASSERT_EQ(file.request(terminal::TCSETSF2), 0);
    ASSERT_EQ(file.request(terminal::FIONREAD), 0);
    EXPECT_EQ(*file.page.at<int32_t>(0), 0);
}

// The driver may run nothing of the port's once unregister_port returns, so it waits for a settings request still
// inside the chip, and later requests fail without reaching the driver
TEST(serial_port_requests, unregistering_waits_out_a_settings_request_in_the_chip) {
    fs::file* f = fs::open(PORT_PATH, fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    {
        user_space_scope scope(page.ctx);
        ASSERT_EQ(fs::ioctl(f, terminal::TCGETS, page.addr), fs::OK);
    }

    g_call.file = f;
    g_call.page = &page;
    g_call.cmd = terminal::TCSETS;

    g_driver.set_line_gate.store_release(1);
    sched::task* setter = start_call(g_call, run_request);
    ASSERT_NOT_NULL(setter);
    ASSERT_TRUE(spin_wait(g_driver.is_in_set_line));

    detach_board();
    sched::task* unregister = start_call(g_unregister_call, run_unregister);
    ASSERT_NOT_NULL(unregister);
    EXPECT_TRUE(blocks_before_deadline(unregister));

    g_driver.set_line_gate.store_release(0);
    EXPECT_TRUE(finish_call(g_call, setter));
    EXPECT_TRUE(finish_call(g_unregister_call, unregister));
    forget_board();

    uint32_t calls = g_driver.set_line_calls;
    {
        user_space_scope scope(page.ctx);
        EXPECT_EQ(fs::ioctl(f, terminal::TCSETS, page.addr), fs::ERR_IO);
    }

    EXPECT_EQ(g_driver.set_line_calls, calls);
    fs::close(f);
}

// An unplug during the first open's chip request fails the open, rather than count a file on a port that has left
TEST(serial_port_requests, an_unplug_during_the_first_open_fails_it) {
    g_driver.set_line_gate.store_release(1);
    sched::task* opener = start_call(g_call, run_open);
    ASSERT_NOT_NULL(opener);
    ASSERT_TRUE(spin_wait(g_driver.is_in_set_line));

    detach_board();
    g_driver.set_line_gate.store_release(0);
    EXPECT_TRUE(finish_call(g_call, opener));
    EXPECT_NULL(g_call.file);
}
