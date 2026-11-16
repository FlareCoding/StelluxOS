#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/serial/serial.h"
#include "tests/serial/fake_port_driver.h"
#include "terminal/tty.h"
#include "fs/fs.h"
#include "fs/file.h"

TEST_SUITE(serial_port_lines);

using serial_test::fake_port_driver;
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

static fake_port_driver g_driver;
static serial::port*    g_port;
static bool             g_is_detached;

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
