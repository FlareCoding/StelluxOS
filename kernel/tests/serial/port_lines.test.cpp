#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/serial/serial.h"
#include "tests/serial/fake_port_driver.h"
#include "terminal/tty.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sync/atomic.h"

TEST_SUITE(serial_port_lines);

using serial_test::fake_port_driver;
using test_helpers::blocks_before_deadline;
using test_helpers::spin_wait;
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

// A board no other test names, and its port's path
static constexpr serial::identity BOARD = {
    .chip = "lnchip",
    .serial_number = "SN3333",
    .location = "8.3",
    .channel = -1,
};

static constexpr const char* PORT_PATH = "/dev/serial/lnchip-3333";

static constexpr uint32_t BOTH_OUTPUTS = terminal::TIOCM_DTR | terminal::TIOCM_RTS;

// A character at a new port's 9600 baud: a start bit, 8 data bits and a stop bit
static constexpr uint64_t CHARACTER_NS = 10ULL * 1000000000ULL / 9600;

// A character at 1200 baud, eight times as long as at a new port's 9600
static constexpr uint32_t B1200 = 0x9;
static constexpr uint64_t CHARACTER_NS_AT_1200 = 8 * CHARACTER_NS;

// TCSBRK's break, and a tenth of a second, TCSBRKP's unit
static constexpr uint64_t BREAK_NS = 250000000;
static constexpr uint64_t TENTH_NS = 100000000;

// What a drain allows beyond the unsent bytes' time
static constexpr uint64_t DRAIN_DEADLINE_MARGIN_NS = 1000000000;

// A count of held bytes that takes longer to send at 9600 baud, about 104 s, than any drain test can wait
static constexpr ssize_t ENDLESS_BYTES = 100000;

// A call a helper task makes on the port, since a wait on the test runner's task never wakes
struct port_call {
    fs::file*              file;
    user_page*             page;
    uint32_t               cmd;
    uint64_t               arg;
    int32_t                result;
    uint64_t               took_ns;
    uint64_t               ended_ns;
    sync::atomic<uint32_t> is_done;
};

static fake_port_driver g_driver;
static serial::port*    g_port;
static bool             g_is_detached;
static port_call        g_call;

static int32_t register_board() {
    g_driver.reset();
    g_port = serial::register_port(&g_driver, BOARD);
    g_driver.port = g_port;
    g_is_detached = false;
    return g_port ? 0 : -1;
}

static int32_t unregister_board() {
    if (!g_port) {
        return 0;
    }

    if (!g_is_detached) {
        serial::detach(g_port);
    }

    serial::unregister_port(g_port);
    g_port = nullptr;
    return 0;
}

// Detaches the port as an unplug does, leaving it registered until the test ends
static void detach_board() {
    serial::detach(g_port);
    g_is_detached = true;
}

BEFORE_EACH(serial_port_lines, register_board);
AFTER_EACH(serial_port_lines, unregister_board);

static fs::file* open_port() {
    return fs::open(PORT_PATH, fs::O_RDWR);
}

// The result of a request whose argument sits at the start of `page`
static int32_t request(fs::file* f, user_page& page, uint32_t cmd) {
    user_space_scope scope(page.ctx);
    return fs::ioctl(f, cmd, page.addr);
}

// The result of a request whose argument is an int at the start of `page`
static int32_t request_int(fs::file* f, user_page& page, uint32_t cmd, int32_t value) {
    *page.at<int32_t>(0) = value;
    return request(f, page, cmd);
}

// The lines TIOCMGET reports, or -1 when it fails
static int32_t modem_lines(fs::file* f, user_page& page) {
    return request(f, page, terminal::TIOCMGET) == fs::OK ? *page.at<int32_t>(0) : -1;
}

// Sets the speed code `code` through `page`, keeping the rest of the settings
static int32_t set_speed(fs::file* f, user_page& page, uint32_t code) {
    int32_t rc = request(f, page, terminal::TCGETS);
    if (rc != fs::OK) {
        return rc;
    }

    auto* settings = page.at<terminal::termios>(0);
    settings->c_cflag = (settings->c_cflag & ~terminal::CBAUD) | code;
    return request(f, page, terminal::TCSETS);
}

static int32_t clear_hupcl(fs::file* f, user_page& page) {
    int32_t rc = request(f, page, terminal::TCGETS);
    if (rc != fs::OK) {
        return rc;
    }

    page.at<terminal::termios>(0)->c_cflag &= ~terminal::HUPCL;
    return request(f, page, terminal::TCSETS);
}

static void finish_timing(uint64_t started_ns) {
    g_call.ended_ns = clock::now_ns();
    g_call.took_ns = g_call.ended_ns - started_ns;
    g_call.is_done.store_release(1);
    sched::exit(0);
}

// A request whose argument is a number, such as TCSBRK's
static void run_number_request(void*) {
    uint64_t started_ns = clock::now_ns();
    g_call.result = fs::ioctl(g_call.file, g_call.cmd, g_call.arg);
    finish_timing(started_ns);
}

static void run_page_request(void*) {
    uint64_t started_ns = clock::now_ns();
    g_call.result = request(g_call.file, *g_call.page, g_call.cmd);
    finish_timing(started_ns);
}

static void run_close(void*) {
    uint64_t started_ns = clock::now_ns();
    fs::close(g_call.file);
    finish_timing(started_ns);
}

// A request or a close runs elevated, as a syscall does
static sched::task* start_call(void (*entry)(void*)) {
    g_call.is_done.store_release(0);
    return start_pinned_task_on_another_cpu(entry, nullptr, "serial_port_call", sched::TASK_FLAG_ELEVATED);
}

static bool finish_call(sched::task* task) {
    bool is_done = spin_wait(g_call.is_done);
    unpin(task);
    return is_done;
}

// On a helper task, makes a request whose argument is a number and waits for it
static bool call_number_request(fs::file* f, uint32_t cmd, uint64_t arg) {
    g_call.file = f;
    g_call.cmd = cmd;
    g_call.arg = arg;
    sched::task* task = start_call(run_number_request);
    return task && finish_call(task);
}

// Scripts the answers tx_pending gives in turn before it reports the chip empty
template <size_t COUNT>
static void script_pending(const ssize_t (&answers)[COUNT]) {
    static_assert(COUNT <= serial_test::MAX_PENDING_ANSWERS, "the fake keeps fewer answers");
    for (size_t i = 0; i < COUNT; i++) {
        g_driver.pending_answers[i] = answers[i];
    }

    g_driver.pending_answer_count = COUNT;
}

// The first open raises DTR and RTS, and a second open leaves them
TEST(serial_port_lines, the_first_open_raises_dtr_and_rts) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(g_driver.outputs, BOTH_OUTPUTS);
    EXPECT_EQ(g_driver.modem_output_calls, 1u);

    fs::file* second = open_port();
    ASSERT_NOT_NULL(second);
    EXPECT_EQ(g_driver.modem_output_calls, 1u);

    fs::close(second);
    fs::close(f);
}

// A port left at B0 opens with DTR and RTS lowered
TEST(serial_port_lines, a_first_open_at_b0_leaves_dtr_and_rts_lowered) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    ASSERT_EQ(set_speed(f, page, terminal::B0), fs::OK);
    fs::close(f);

    uint32_t calls_before = g_driver.modem_output_calls;
    f = open_port();
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(g_driver.modem_output_calls, calls_before);
    EXPECT_EQ(g_driver.outputs, 0u);

    fs::close(f);
}

// A first open whose DTR and RTS request fails leaves the port closed, so the next open is a first open again
TEST(serial_port_lines, a_failed_raise_fails_the_first_open) {
    g_driver.control_result = serial::ERR_IO;
    EXPECT_NULL(open_port());

    g_driver.control_result = serial::OK;
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    EXPECT_EQ(g_driver.set_line_calls, 2u);
    EXPECT_EQ(g_driver.outputs, BOTH_OUTPUTS);

    fs::close(f);
}

// HUPCL, set on a new port, has the last close lower DTR and RTS, and without it they stay raised
TEST(serial_port_lines, the_last_close_lowers_dtr_and_rts_under_hupcl) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    fs::close(f);
    EXPECT_EQ(g_driver.outputs, 0u);

    f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    ASSERT_EQ(clear_hupcl(f, page), fs::OK);

    fs::close(f);
    EXPECT_EQ(g_driver.outputs, BOTH_OUTPUTS);
}

// B0 lowers DTR and RTS at once, keeping the chip's speed, and leaving B0 raises them
TEST(serial_port_lines, b0_lowers_dtr_and_rts_and_leaving_it_raises_them) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(set_speed(f, page, terminal::B0), fs::OK);
    EXPECT_EQ(g_driver.outputs, 0u);
    EXPECT_EQ(g_driver.line.baud, 9600u);

    ASSERT_EQ(set_speed(f, page, terminal::B115200), fs::OK);
    EXPECT_EQ(g_driver.outputs, BOTH_OUTPUTS);
    EXPECT_EQ(g_driver.line.baud, 115200u);

    fs::close(f);
}

// TIOCMGET reports DTR and RTS as last set with the chip's inputs
TEST(serial_port_lines, tiocmget_reports_the_outputs_as_set_and_the_chips_inputs) {
    g_driver.inputs = terminal::TIOCM_CTS | terminal::TIOCM_DSR;
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(modem_lines(f, page), static_cast<int32_t>(BOTH_OUTPUTS | terminal::TIOCM_CTS | terminal::TIOCM_DSR));
    ASSERT_EQ(request_int(f, page, terminal::TIOCMBIC, terminal::TIOCM_DTR), fs::OK);
    EXPECT_EQ(modem_lines(f, page),
              static_cast<int32_t>(terminal::TIOCM_RTS | terminal::TIOCM_CTS | terminal::TIOCM_DSR));

    fs::close(f);
}

// Each of TIOCMSET, TIOCMBIS and TIOCMBIC changes both lines in one request, and bits other than DTR and RTS change
// nothing
TEST(serial_port_lines, the_tiocm_requests_set_both_lines_in_one_request) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    uint32_t calls = g_driver.modem_output_calls;

    ASSERT_EQ(request_int(f, page, terminal::TIOCMSET, terminal::TIOCM_DTR | terminal::TIOCM_CTS), fs::OK);
    EXPECT_EQ(g_driver.outputs, terminal::TIOCM_DTR);
    ASSERT_EQ(request_int(f, page, terminal::TIOCMBIS, terminal::TIOCM_RTS), fs::OK);
    EXPECT_EQ(g_driver.outputs, BOTH_OUTPUTS);

    ASSERT_EQ(request_int(f, page, terminal::TIOCMBIC, static_cast<int32_t>(BOTH_OUTPUTS)), fs::OK);
    EXPECT_EQ(g_driver.outputs, 0u);
    EXPECT_EQ(g_driver.modem_output_calls, calls + 3);

    fs::close(f);
}

// A modem line request the chip fails returns EIO, and the lines read back as last set
TEST(serial_port_lines, a_failed_modem_request_keeps_the_lines_as_last_set) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());

    g_driver.control_result = serial::ERR_IO;
    EXPECT_EQ(request_int(f, page, terminal::TIOCMSET, 0), fs::ERR_IO);
    EXPECT_EQ(request(f, page, terminal::TIOCMGET), fs::ERR_IO);

    g_driver.control_result = serial::OK;
    EXPECT_EQ(modem_lines(f, page), static_cast<int32_t>(BOTH_OUTPUTS));

    fs::close(f);
}

TEST(serial_port_lines, tiocsbrk_and_tioccbrk_start_and_end_a_break) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    EXPECT_EQ(fs::ioctl(f, terminal::TIOCSBRK, 0), fs::OK);
    EXPECT_TRUE(g_driver.is_breaking);
    EXPECT_EQ(fs::ioctl(f, terminal::TIOCCBRK, 0), fs::OK);
    EXPECT_FALSE(g_driver.is_breaking);

    fs::close(f);
}

TEST(serial_port_lines, the_last_close_ends_a_break) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fs::ioctl(f, terminal::TIOCSBRK, 0), fs::OK);

    fs::close(f);
    EXPECT_FALSE(g_driver.is_breaking);
}

// A chip holds what it has to send while it breaks, so the last close ends the break before its drain
TEST(serial_port_lines, the_last_close_ends_a_break_before_it_drains) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fs::ioctl(f, terminal::TIOCSBRK, 0), fs::OK);
    const ssize_t answers[] = {ENDLESS_BYTES};
    script_pending(answers);

    g_call.file = f;
    sched::task* closer = start_call(run_close);
    ASSERT_NOT_NULL(closer);
    ASSERT_TRUE(blocks_before_deadline(closer));
    EXPECT_FALSE(g_driver.is_breaking);

    detach_board();
    EXPECT_TRUE(finish_call(closer));
}

// A break the last close could not end is ended by the next first open
TEST(serial_port_lines, the_first_open_ends_a_break_the_last_close_could_not) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fs::ioctl(f, terminal::TIOCSBRK, 0), fs::OK);

    g_driver.control_result = serial::ERR_IO;
    fs::close(f);
    EXPECT_TRUE(g_driver.is_breaking);

    g_driver.control_result = serial::OK;
    f = open_port();
    ASSERT_NOT_NULL(f);
    EXPECT_FALSE(g_driver.is_breaking);

    fs::close(f);
}

// TCSBRK with 0 drains, then breaks for 250 ms
TEST(serial_port_lines, tcsbrk_0_drains_then_breaks_for_250_ms) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 0));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(1));
    EXPECT_EQ(g_driver.break_calls, 2u);
    EXPECT_FALSE(g_driver.is_breaking);
    EXPECT_GE(g_driver.break_ended_ns - g_driver.break_started_ns, BREAK_NS);

    fs::close(f);
}

// TCSBRK with any other argument is tcdrain, which drains without a break
TEST(serial_port_lines, tcsbrk_with_a_nonzero_argument_only_drains) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 1));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(1));
    EXPECT_EQ(g_driver.break_calls, 0u);

    fs::close(f);
}

TEST(serial_port_lines, tcsbrkp_breaks_for_its_tenths_of_a_second) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRKP, 1));
    EXPECT_EQ(g_call.result, fs::OK);
    uint64_t break_ns = g_driver.break_ended_ns - g_driver.break_started_ns;
    EXPECT_GE(break_ns, TENTH_NS);
    EXPECT_LT(break_ns, BREAK_NS);

    fs::close(f);
}

TEST(serial_port_lines, tcsbrkp_0_breaks_for_250_ms) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRKP, 0));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_GE(g_driver.break_ended_ns - g_driver.break_started_ns, BREAK_NS);

    fs::close(f);
}

// TCSBRKP holds the write lock through its break, so a break past 5 s, read from all 64 bits of the argument, is
// refused before the drain
TEST(serial_port_lines, a_tcsbrkp_past_5_s_fails_with_einval_and_sends_nothing) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRKP, 51));
    EXPECT_EQ(g_call.result, fs::ERR_INVAL);
    EXPECT_TRUE(call_number_request(f, terminal::TCSBRKP, 0x100000001ULL));
    EXPECT_EQ(g_call.result, fs::ERR_INVAL);
    EXPECT_EQ(g_driver.break_calls, 0u);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(0));

    fs::close(f);
}

// A signal ends a break early with EINTR, and the request ending the break still goes out
TEST(serial_port_lines, a_signal_ends_a_break_early_and_the_break_still_ends) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    g_call.file = f;
    g_call.cmd = terminal::TCSBRKP;
    g_call.arg = 50;

    sched::task* breaker = start_call(run_number_request);
    ASSERT_NOT_NULL(breaker);
    ASSERT_TRUE(blocks_before_deadline(breaker));
    EXPECT_TRUE(g_driver.is_breaking);

    RUN_ELEVATED(sched::force_wake_for_kill(breaker));
    EXPECT_TRUE(finish_call(breaker));
    EXPECT_EQ(g_call.result, fs::ERR_INTR);
    EXPECT_FALSE(g_driver.is_breaking);
    EXPECT_LT(g_call.took_ns, 50 * TENTH_NS);

    fs::close(f);
}

// A drain waits out the bytes the chip reports at the line's speed, then asks again
TEST(serial_port_lines, a_drain_waits_out_the_bytes_the_chip_holds) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    const ssize_t answers[] = {96};
    script_pending(answers);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 1));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(2));
    EXPECT_GE(g_call.took_ns, 96 * CHARACTER_NS);

    fs::close(f);
}

// Bytes the chip takes at 1200 baud, which it sends over 200 ms
static constexpr size_t TAKEN_BYTES = 24;

// A chip's count leaves out the byte leaving its UART, so a drain before a break or a settings change waits one
// character more once the count reaches 0, while the chip may still be sending what it took
TEST(serial_port_lines, a_drain_waits_a_character_for_the_byte_the_count_leaves_out) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    ASSERT_EQ(set_speed(f, page, B1200), fs::OK);

    uint8_t bytes[TAKEN_BYTES] = {};
    ASSERT_EQ(fs::write(f, bytes, TAKEN_BYTES, fs::O_NONBLOCK), static_cast<ssize_t>(TAKEN_BYTES));
    g_driver.end_send(TAKEN_BYTES);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 1));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(1));
    EXPECT_GE(g_call.took_ns, CHARACTER_NS_AT_1200);

    fs::close(f);
}

// A chip that keeps reporting bytes past the drain's deadline fails the drain with EIO
TEST(serial_port_lines, a_drain_past_its_deadline_fails_with_eio) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    const ssize_t answers[] = {ENDLESS_BYTES, ENDLESS_BYTES};
    script_pending(answers);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 1));
    EXPECT_EQ(g_call.result, fs::ERR_IO);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(2));

    fs::close(f);
}

// An unplug ends a last close's drain at once, though the chip still reports bytes to send
TEST(serial_port_lines, an_unplug_ends_the_last_closes_drain_at_once) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    const ssize_t answers[] = {ENDLESS_BYTES};
    script_pending(answers);

    g_call.file = f;
    sched::task* closer = start_call(run_close);
    ASSERT_NOT_NULL(closer);
    ASSERT_TRUE(blocks_before_deadline(closer));

    detach_board();
    EXPECT_TRUE(finish_call(closer));
    EXPECT_LT(g_call.took_ns, DRAIN_DEADLINE_MARGIN_NS / 2);
}

TEST(serial_port_lines, an_unplug_fails_a_waiting_tcdrain_with_eio) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    const ssize_t answers[] = {ENDLESS_BYTES};
    script_pending(answers);

    g_call.file = f;
    g_call.cmd = terminal::TCSBRK;
    g_call.arg = 1;
    sched::task* drainer = start_call(run_number_request);
    ASSERT_NOT_NULL(drainer);
    ASSERT_TRUE(blocks_before_deadline(drainer));

    detach_board();
    EXPECT_TRUE(finish_call(drainer));
    EXPECT_EQ(g_call.result, fs::ERR_IO);
    EXPECT_LT(g_call.took_ns, DRAIN_DEADLINE_MARGIN_NS / 2);

    fs::close(f);
}

// Bytes that take 1.25 s at 9600 baud, and most of them, which the chip reports still holding
static constexpr size_t SLOW_SEND_BYTES = 1200;
static constexpr ssize_t MOST_OF_A_SLOW_SEND = 1100;
static uint8_t g_slow_send[SLOW_SEND_BYTES];

// The drain's deadline allows for the bytes the chip took as well as the margin, so a chip still sending them a second
// after the drain starts is waited for
TEST(serial_port_lines, a_drain_allows_for_the_bytes_the_chip_took) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    const ssize_t answers[] = {MOST_OF_A_SLOW_SEND, 1};
    script_pending(answers);

    ASSERT_EQ(fs::write(f, g_slow_send, SLOW_SEND_BYTES, fs::O_NONBLOCK), static_cast<ssize_t>(SLOW_SEND_BYTES));
    g_driver.end_send(SLOW_SEND_BYTES);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 1));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(3));

    fs::close(f);
}

// A drain counts the bytes the chip holds at the speed the settings set last
TEST(serial_port_lines, a_drain_counts_characters_at_the_speed_set_last) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    ASSERT_EQ(set_speed(f, page, B1200), fs::OK);

    const ssize_t answers[] = {24};
    script_pending(answers);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 1));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_GE(g_call.took_ns, 24 * CHARACTER_NS_AT_1200);

    fs::close(f);
}

TEST(serial_port_lines, a_failed_tx_pending_fails_the_drain_with_eio) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    const ssize_t answers[] = {serial::ERR_IO};
    script_pending(answers);

    EXPECT_TRUE(call_number_request(f, terminal::TCSBRK, 1));
    EXPECT_EQ(g_call.result, fs::ERR_IO);

    fs::close(f);
}

// TCSETSW drains the chip too, not only the send
TEST(serial_port_lines, tcsetsw_drains_the_chip) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    ASSERT_EQ(request(f, page, terminal::TCGETS), fs::OK);

    const ssize_t answers[] = {48};
    script_pending(answers);

    g_call.file = f;
    g_call.page = &page;
    g_call.cmd = terminal::TCSETSW;
    sched::task* setter = start_call(run_page_request);
    ASSERT_NOT_NULL(setter);

    EXPECT_TRUE(finish_call(setter));
    EXPECT_EQ(g_call.result, fs::OK);
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(2));
    EXPECT_GE(g_call.took_ns, 48 * CHARACTER_NS);

    fs::close(f);
}

// The last close drains the chip and, under HUPCL, lowers the lines
TEST(serial_port_lines, the_last_close_drains_the_chip) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    const ssize_t answers[] = {48};
    script_pending(answers);

    g_call.file = f;
    sched::task* closer = start_call(run_close);
    ASSERT_NOT_NULL(closer);

    EXPECT_TRUE(finish_call(closer));
    EXPECT_EQ(g_driver.tx_pending_calls, static_cast<size_t>(2));
    EXPECT_GE(g_call.took_ns, 48 * CHARACTER_NS);
    EXPECT_EQ(g_driver.outputs, 0u);
}
