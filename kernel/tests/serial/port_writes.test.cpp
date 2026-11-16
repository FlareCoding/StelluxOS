#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/serial/serial.h"
#include "tests/serial/fake_port_driver.h"
#include "terminal/tty.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_poll.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "common/string.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sync/atomic.h"
#include "sync/poll.h"

TEST_SUITE(serial_port_writes);

using serial_test::fake_port_driver;
using test_helpers::blocks_before_deadline;
using test_helpers::spin_wait;
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

// A board no other test names, and its port's path
static constexpr serial::identity BOARD = {
    .chip = "wrchip",
    .serial_number = "SN5555",
    .location = "8.2",
    .channel = -1,
};

static constexpr const char* PORT_PATH = "/dev/serial/wrchip-5555";

// More than one send carries
static constexpr size_t TWO_SENDS_BYTES = serial::SEND_BYTES + 904;

// The speed code of 3 Mbaud, at which a send's bytes take little of a deadline that adds a second to them
static constexpr uint32_t B3000000 = 0x100D;

// Far longer than a few bytes take at 9600 baud, and far shorter than the second a send's deadline adds to them
static constexpr uint64_t LONGER_THAN_THE_BYTES_NS = 50000000;

// What a wait for the send allows beyond its bytes' time
static constexpr uint64_t SEND_DEADLINE_MARGIN_NS = 1000000000;

// Longer than any wake a poller waits for, so one still waiting this long has missed its wake
static constexpr uint64_t POLL_WAIT_MS = 2000;
static constexpr uint64_t NS_PER_MS = 1000000;

struct user_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

// A call a helper task makes on the port, since a wait on the test runner's task never wakes
struct port_call {
    fs::file*              file;
    user_page*             page;
    uint32_t               cmd;
    size_t                 count;
    ssize_t                result;
    uint64_t               took_ns;
    sync::atomic<uint32_t> is_ready; // set by a call that waits for `is_go` once it is ready to start
    sync::atomic<uint32_t> is_go;    // a call that waits for it starts once the test sets it
    sync::atomic<uint32_t> is_done;
};

static fake_port_driver g_driver;
static serial::port*    g_port;
static bool             g_is_detached;
static uint8_t          g_bytes[TWO_SENDS_BYTES];
static port_call        g_call;
static port_call        g_other_call;

static int32_t register_board() {
    g_driver.reset();
    g_port = serial::register_port(&g_driver, BOARD);
    g_driver.port = g_port;
    g_is_detached = false;
    for (size_t i = 0; i < TWO_SENDS_BYTES; i++) {
        g_bytes[i] = static_cast<uint8_t>(i);
    }

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

BEFORE_EACH(serial_port_writes, register_board);
AFTER_EACH(serial_port_writes, unregister_board);

static fs::file* open_port() {
    return fs::open(PORT_PATH, fs::O_RDWR);
}

static ssize_t write_now(fs::file* f, size_t count) {
    return fs::write(f, g_bytes, count, fs::O_NONBLOCK);
}

static uint32_t poll_events(fs::file* f) {
    uint32_t events = 0;
    RUN_ELEVATED(events = f->get_node()->poll(f, nullptr));
    return events;
}

// The result of a request whose argument sits at the start of `page`
static int32_t request(fs::file* f, user_page& page, uint32_t cmd) {
    user_space_scope scope(page.ctx);
    return fs::ioctl(f, cmd, page.addr);
}

static int32_t bytes_in_send(fs::file* f, user_page& page) {
    EXPECT_EQ(request(f, page, terminal::TIOCOUTQ), fs::OK);
    return *page.at<int32_t>(0);
}

// Spins the test runner's task, which cannot sleep, for `duration_ns`
static void spin_for(uint64_t duration_ns) {
    uint64_t until = clock::now_ns() + duration_ns;
    while (clock::now_ns() < until) {
    }
}

static void finish_call_task(port_call* call, uint64_t started_ns) {
    call->took_ns = clock::now_ns() - started_ns;
    call->is_done.store_release(1);
    sched::exit(0);
}

static void run_blocking_write(void* arg) {
    auto* call = static_cast<port_call*>(arg);
    uint64_t started_ns = clock::now_ns();
    call->result = fs::write(call->file, g_bytes, call->count, 0);
    finish_call_task(call, started_ns);
}

static void run_request(void* arg) {
    auto* call = static_cast<port_call*>(arg);
    uint64_t started_ns = clock::now_ns();
    call->result = request(call->file, *call->page, call->cmd);
    finish_call_task(call, started_ns);
}

// A request whose argument is a number, such as TCFLSH's
static void run_number_request(void* arg) {
    auto* call = static_cast<port_call*>(arg);
    uint64_t started_ns = clock::now_ns();
    call->result = fs::ioctl(call->file, call->cmd, call->count);
    finish_call_task(call, started_ns);
}

static void run_close(void* arg) {
    auto* call = static_cast<port_call*>(arg);
    uint64_t started_ns = clock::now_ns();
    fs::close(call->file);
    finish_call_task(call, started_ns);
}

// A close that starts once the test sets `is_go`, so the test can mark a kill pending first
static void run_close_on_go(void* arg) {
    auto* call = static_cast<port_call*>(arg);
    while (!call->is_go.load_acquire()) {
        test_helpers::brief_delay();
    }

    run_close(arg);
}

// Waits in poll for the port to turn writable, through a handle of the task's own, as a program does. The open comes
// before `is_go`, since an open takes the control lock, which the test may then hold
static void run_poller(void* arg) {
    auto* call = static_cast<port_call*>(arg);
    sched::task* self = sched::current();
    resource::handle_t handle = -1;
    call->result = -1;

    bool is_open = resource::open(self, PORT_PATH, fs::O_RDWR, &handle) == resource::OK;
    call->is_ready.store_release(1);
    while (!call->is_go.load_acquire()) {
        test_helpers::brief_delay();
    }

    uint64_t started_ns = clock::now_ns();
    if (is_open) {
        *call->page->at<user_pollfd>(0) = {handle, static_cast<int16_t>(sync::POLL_OUT), 0};
        {
            user_space_scope scope(call->page->ctx);
            call->result = sys_poll(call->page->addr, 1, POLL_WAIT_MS, 0, 0, 0);
        }

        (void)resource::close(self, handle);
    }

    finish_call_task(call, started_ns);
}

static void run_unregister(void* arg) {
    auto* call = static_cast<port_call*>(arg);
    uint64_t started_ns = clock::now_ns();
    serial::unregister_port(g_port);
    finish_call_task(call, started_ns);
}

// A writer runs lowered as a driver's task does, while a request or a close runs elevated as a syscall does
static sched::task* start_call(port_call& call, void (*entry)(void*), uint32_t flags = 0) {
    call.is_ready.store_release(0);
    call.is_go.store_release(0);
    call.is_done.store_release(0);
    return start_pinned_task_on_another_cpu(entry, &call, "serial_port_call", flags);
}

static bool finish_call(port_call& call, sched::task* task) {
    bool is_done = spin_wait(call.is_done);
    unpin(task);
    return is_done;
}

// Sets the port to 3 Mbaud through `page`, so a wait for a send gives up about a second after it starts
static void set_fast_speed(fs::file* f, user_page& page) {
    ASSERT_EQ(request(f, page, terminal::TCGETS), fs::OK);
    auto* settings = page.at<terminal::termios>(0);
    settings->c_cflag = (settings->c_cflag & ~terminal::CBAUD) | B3000000;
    ASSERT_EQ(request(f, page, terminal::TCSETS), fs::OK);
}

// A write returns once its bytes are in the port's send, without waiting for the chip to take them
TEST(serial_port_writes, a_write_puts_its_bytes_in_one_send) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(fs::write(f, g_bytes, 5, 0), 5);
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 1u);
    EXPECT_EQ(g_driver.sent_length, static_cast<size_t>(5));
    EXPECT_EQ(string::memcmp(g_driver.sent, g_bytes, 5), 0);
    EXPECT_EQ(bytes_in_send(f, page), 5);

    g_driver.end_send(5);
    EXPECT_EQ(bytes_in_send(f, page), 0);

    fs::close(f);
}

// A send carries at most SEND_BYTES, and one that ends before the driver's call returns frees the port for the next
TEST(serial_port_writes, a_long_write_takes_a_send_per_4_kb) {
    g_driver.ends_sends_at_once = true;
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    EXPECT_EQ(fs::write(f, g_bytes, TWO_SENDS_BYTES, 0), static_cast<ssize_t>(TWO_SENDS_BYTES));
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 2u);
    EXPECT_EQ(g_driver.sent_length, TWO_SENDS_BYTES - serial::SEND_BYTES);
    EXPECT_EQ(string::memcmp(g_driver.sent, g_bytes + serial::SEND_BYTES, g_driver.sent_length), 0);

    // A send that ended before its call returned is never cancelled
    fs::close(f);
    EXPECT_EQ(g_driver.cancels, 0u);
}

// The program's buffer is copied in through the syscall layer, as a program's write is
TEST(serial_port_writes, a_program_writes_from_its_own_memory) {
    g_driver.ends_sends_at_once = true;
    user_page page;
    ASSERT_TRUE(page.ready());
    resource::handle_t handle = -1;
    ASSERT_EQ(resource::open(sched::current(), PORT_PATH, fs::O_RDWR, &handle), resource::OK);

    string::memcpy(page.at<uint8_t>(0), "board", 5);
    int64_t written = 0;
    {
        user_space_scope scope(page.ctx);
        written = sys_write(static_cast<uint64_t>(handle), page.addr, 5, 0, 0, 0);
    }

    EXPECT_EQ(written, 5);
    EXPECT_EQ(string::memcmp(g_driver.sent, "board", 5), 0);
    (void)resource::close(sched::current(), handle);
}

// While a send is in flight a non-blocking write returns EAGAIN and poll reports no POLLOUT, until the send ends
TEST(serial_port_writes, a_nonblocking_write_gets_eagain_while_a_send_is_in_flight) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(write_now(f, 3), 3);

    EXPECT_EQ(write_now(f, 3), static_cast<ssize_t>(fs::ERR_AGAIN));
    EXPECT_EQ(poll_events(f) & sync::POLL_OUT, 0u);

    g_driver.end_send(3);
    EXPECT_EQ(poll_events(f) & sync::POLL_OUT, sync::POLL_OUT);
    EXPECT_EQ(write_now(f, 3), 3);
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 2u);

    g_driver.end_send(3);
    fs::close(f);
}

// A blocking writer waits for the send in flight holding no lock, so another file's TCSETSW, which takes the write
// lock, gives up with EIO at its deadline rather than wait behind the writer
TEST(serial_port_writes, a_blocking_writer_waits_for_the_send_holding_no_lock) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    fs::file* other = open_port();
    ASSERT_NOT_NULL(other);
    user_page page;
    ASSERT_TRUE(page.ready());

    set_fast_speed(f, page);
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    g_call.count = 7;
    sched::task* writer = start_call(g_call, run_blocking_write);
    ASSERT_NOT_NULL(writer);
    EXPECT_TRUE(blocks_before_deadline(writer));

    ASSERT_EQ(request(other, page, terminal::TCGETS), fs::OK);
    g_other_call.file = other;
    g_other_call.page = &page;
    g_other_call.cmd = terminal::TCSETSW;

    sched::task* setter = start_call(g_other_call, run_request, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(setter);
    EXPECT_TRUE(finish_call(g_other_call, setter));
    EXPECT_EQ(g_other_call.result, static_cast<ssize_t>(fs::ERR_IO));

    g_driver.end_send(3);
    bool is_done = finish_call(g_call, writer);
    EXPECT_TRUE(is_done);
    EXPECT_EQ(g_call.result, 7);
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 2u);

    g_driver.end_send(7);
    fs::close(other);
    fs::close(f);
}

// A kill ends the wait of a writer whose bytes never reached a send
TEST(serial_port_writes, a_kill_ends_a_waiting_write_with_eintr) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    g_call.count = 7;
    sched::task* writer = start_call(g_call, run_blocking_write);
    ASSERT_NOT_NULL(writer);
    ASSERT_TRUE(blocks_before_deadline(writer));

    RUN_ELEVATED(sched::force_wake_for_kill(writer));
    EXPECT_TRUE(finish_call(g_call, writer));
    EXPECT_EQ(g_call.result, static_cast<ssize_t>(fs::ERR_INTR));
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 1u);

    g_driver.end_send(3);
    fs::close(f);
}

TEST(serial_port_writes, tcoflush_cancels_the_send_in_flight) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    ASSERT_EQ(write_now(f, 9), 9);

    {
        user_space_scope scope(page.ctx);
        EXPECT_EQ(fs::ioctl(f, terminal::TCFLSH, terminal::TCOFLUSH), fs::OK);
    }

    EXPECT_EQ(g_driver.cancels, 1u);
    EXPECT_EQ(bytes_in_send(f, page), 0);
    EXPECT_EQ(poll_events(f) & sync::POLL_OUT, sync::POLL_OUT);

    fs::close(f);
}

// A flush during a send whose driver call has not returned waits for the call, then cancels the send
TEST(serial_port_writes, tcoflush_cancels_a_send_still_starting_in_the_driver) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    g_driver.send_gate.store_release(1);
    g_call.file = f;
    g_call.count = 3;
    sched::task* writer = start_call(g_call, run_blocking_write);
    ASSERT_NOT_NULL(writer);
    ASSERT_TRUE(spin_wait(g_driver.is_in_send));

    g_other_call.file = f;
    g_other_call.cmd = terminal::TCFLSH;
    g_other_call.count = terminal::TCOFLUSH;
    sched::task* flusher = start_call(g_other_call, run_number_request, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(flusher);
    EXPECT_TRUE(blocks_before_deadline(flusher));

    g_driver.send_gate.store_release(0);
    EXPECT_TRUE(finish_call(g_call, writer));
    EXPECT_EQ(g_call.result, 3);
    EXPECT_TRUE(finish_call(g_other_call, flusher));
    EXPECT_EQ(g_other_call.result, static_cast<ssize_t>(fs::OK));

    EXPECT_EQ(g_driver.cancels, 1u);
    EXPECT_FALSE(g_driver.is_sending);
    fs::close(f);
}

// A send that never starts fails only its own write, while one that fails fails every later write
TEST(serial_port_writes, a_failed_send_fails_later_writes_with_eio) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    g_driver.send_result = serial::ERR_IO;
    EXPECT_EQ(write_now(f, 4), static_cast<ssize_t>(fs::ERR_IO));
    g_driver.send_result = serial::OK;
    ASSERT_EQ(write_now(f, 4), 4);

    g_driver.end_send(serial::ERR_IO);
    EXPECT_EQ(write_now(f, 4), static_cast<ssize_t>(fs::ERR_IO));
    EXPECT_EQ(fs::write(f, g_bytes, 4, 0), static_cast<ssize_t>(fs::ERR_IO));
    EXPECT_EQ(poll_events(f) & (sync::POLL_OUT | sync::POLL_ERR), sync::POLL_OUT);
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 1u);

    fs::close(f);
}

TEST(serial_port_writes, writes_after_an_unplug_fail_with_eio) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    detach_board();
    EXPECT_EQ(write_now(f, 4), static_cast<ssize_t>(fs::ERR_IO));
    EXPECT_EQ(poll_events(f), sync::POLL_OUT | sync::POLL_HUP);
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 0u);

    fs::close(f);
}

// TCSETSW lets the send in flight end before it changes the settings, waiting past the time its bytes take
TEST(serial_port_writes, tcsetsw_waits_for_the_send_in_flight) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(request(f, page, terminal::TCGETS), fs::OK);
    auto* settings = page.at<terminal::termios>(0);
    settings->c_cflag = (settings->c_cflag & ~terminal::CBAUD) | B3000000;
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    g_call.page = &page;
    g_call.cmd = terminal::TCSETSW;
    sched::task* setter = start_call(g_call, run_request, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(setter);

    EXPECT_TRUE(blocks_before_deadline(setter));
    EXPECT_EQ(g_driver.line.baud, 9600u);

    spin_for(LONGER_THAN_THE_BYTES_NS);
    g_driver.end_send(3);
    EXPECT_TRUE(finish_call(g_call, setter));
    EXPECT_EQ(g_call.result, static_cast<ssize_t>(fs::OK));
    EXPECT_EQ(g_driver.line.baud, 3000000u);

    fs::close(f);
}

// A send the chip never ends holds TCSETSW until its deadline, when it fails with EIO and applies nothing
TEST(serial_port_writes, tcsetsw_fails_with_eio_at_its_deadline) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());

    set_fast_speed(f, page);
    ASSERT_EQ(request(f, page, terminal::TCGETS), fs::OK);
    page.at<terminal::termios>(0)->c_cflag |= terminal::CSTOPB;
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    g_call.page = &page;
    g_call.cmd = terminal::TCSETSW;
    sched::task* setter = start_call(g_call, run_request, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(setter);

    EXPECT_TRUE(finish_call(g_call, setter));
    EXPECT_EQ(g_call.result, static_cast<ssize_t>(fs::ERR_IO));
    EXPECT_EQ(g_driver.line.stop_bits, 1);

    g_driver.end_send(3);
    fs::close(f);
}

// The last close waits for the send up to its deadline, then cancels it, so a chip that stops taking bytes holds the
// close no longer than that
TEST(serial_port_writes, the_last_close_cancels_a_send_past_its_deadline) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    set_fast_speed(f, page);
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    sched::task* closer = start_call(g_call, run_close, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(closer);
    EXPECT_TRUE(finish_call(g_call, closer));

    EXPECT_EQ(g_driver.cancels, 1u);
    EXPECT_FALSE(g_driver.is_sending);
    EXPECT_GE(g_call.took_ns, SEND_DEADLINE_MARGIN_NS);
}

TEST(serial_port_writes, a_close_that_is_not_the_last_leaves_the_send_alone) {
    fs::file* writer = open_port();
    fs::file* other = open_port();
    ASSERT_NOT_NULL(writer);
    ASSERT_NOT_NULL(other);
    ASSERT_EQ(write_now(writer, 3), 3);

    fs::close(other);
    EXPECT_EQ(g_driver.cancels, 0u);
    EXPECT_TRUE(g_driver.is_sending);

    g_driver.end_send(3);
    fs::close(writer);
}

// The last close waits for a send the chip is still taking, and leaves it to end rather than cancel it
TEST(serial_port_writes, the_last_close_waits_for_the_send_in_flight) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    sched::task* closer = start_call(g_call, run_close, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(closer);
    EXPECT_TRUE(blocks_before_deadline(closer));

    g_driver.end_send(3);
    EXPECT_TRUE(finish_call(g_call, closer));
    EXPECT_EQ(g_driver.cancels, 0u);
}

// A killed program's last close skips the wait and cancels the send at once
TEST(serial_port_writes, a_last_close_with_a_kill_pending_cancels_the_send_at_once) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    sched::task* closer = start_call(g_call, run_close_on_go, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(closer);
    RUN_ELEVATED(sched::force_wake_for_kill(closer));
    g_call.is_go.store_release(1);

    EXPECT_TRUE(finish_call(g_call, closer));
    EXPECT_EQ(g_driver.cancels, 1u);
    EXPECT_LT(g_call.took_ns, SEND_DEADLINE_MARGIN_NS);
}

// The driver may run nothing of the port's once unregister_port returns, so it waits for a send still starting in the
// driver, and later writes fail without reaching the driver
TEST(serial_port_writes, unregistering_waits_out_a_send_starting_in_the_driver) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);

    g_driver.send_gate.store_release(1);
    g_call.file = f;
    g_call.count = 3;
    sched::task* writer = start_call(g_call, run_blocking_write);
    ASSERT_NOT_NULL(writer);
    ASSERT_TRUE(spin_wait(g_driver.is_in_send));

    detach_board();
    sched::task* unregister = start_call(g_other_call, run_unregister);
    ASSERT_NOT_NULL(unregister);
    EXPECT_TRUE(blocks_before_deadline(unregister));

    g_driver.send_gate.store_release(0);
    EXPECT_TRUE(finish_call(g_call, writer));
    EXPECT_EQ(g_call.result, 3);
    EXPECT_TRUE(finish_call(g_other_call, unregister));
    g_port = nullptr;

    EXPECT_EQ(write_now(f, 3), static_cast<ssize_t>(fs::ERR_IO));
    EXPECT_EQ(g_driver.started_sends.load_relaxed(), 1u);
    fs::close(f);
}

// TCSETSW holds the write lock inside its chip request with no send in flight, keeping writes out and the port
// unwritable until its release wakes a poller. The chip fails it, so no settings change wakes the poller first
TEST(serial_port_writes, a_held_write_lock_makes_writes_wait_without_a_send) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    user_page poll_page;
    ASSERT_TRUE(poll_page.ready());

    ASSERT_EQ(request(f, page, terminal::TCGETS), fs::OK);
    g_other_call.page = &poll_page;
    sched::task* poller = start_call(g_other_call, run_poller, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(poller);
    ASSERT_TRUE(spin_wait(g_other_call.is_ready));

    g_call.file = f;
    g_call.page = &page;
    g_call.cmd = terminal::TCSETSW;
    g_driver.set_line_result = serial::ERR_IO;

    g_driver.set_line_gate.store_release(1);
    sched::task* setter = start_call(g_call, run_request, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(setter);
    ASSERT_TRUE(spin_wait(g_driver.is_in_set_line));

    EXPECT_EQ(write_now(f, 3), static_cast<ssize_t>(fs::ERR_AGAIN));
    EXPECT_EQ(poll_events(f) & sync::POLL_OUT, 0u);
    g_other_call.is_go.store_release(1);
    EXPECT_TRUE(blocks_before_deadline(poller));

    g_driver.set_line_gate.store_release(0);
    EXPECT_TRUE(finish_call(g_call, setter));
    EXPECT_EQ(g_call.result, static_cast<ssize_t>(fs::ERR_IO));

    EXPECT_TRUE(finish_call(g_other_call, poller));
    EXPECT_EQ(g_other_call.result, 1);
    EXPECT_EQ(poll_page.at<user_pollfd>(0)->revents, static_cast<int16_t>(sync::POLL_OUT));
    EXPECT_LT(g_other_call.took_ns, POLL_WAIT_MS * NS_PER_MS);

    fs::close(f);
}

// No send starts while a cancel is still running, though the cancelled send has ended, so the driver's cancel never
// meets a send that is starting
TEST(serial_port_writes, a_write_waits_for_a_cancel_to_finish) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(write_now(f, 3), 3);

    g_call.file = f;
    g_call.cmd = terminal::TCFLSH;
    g_call.count = terminal::TCOFLUSH;

    g_driver.cancel_gate.store_release(1);
    sched::task* flusher = start_call(g_call, run_number_request, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(flusher);
    ASSERT_TRUE(spin_wait(g_driver.is_in_cancel));

    EXPECT_EQ(write_now(f, 3), static_cast<ssize_t>(fs::ERR_AGAIN));
    EXPECT_EQ(poll_events(f) & sync::POLL_OUT, 0u);

    g_driver.cancel_gate.store_release(0);
    EXPECT_TRUE(finish_call(g_call, flusher));
    EXPECT_EQ(write_now(f, 3), 3);

    g_driver.end_send(3);
    fs::close(f);
}

// TCIOFLUSH cancels the send that TCSETSF and TCSETSW2 wait for and empties the receive queue, after which they apply
TEST(serial_port_writes, tcioflush_frees_the_requests_that_wait_for_a_send) {
    fs::file* f = open_port();
    ASSERT_NOT_NULL(f);
    user_page page;
    ASSERT_TRUE(page.ready());
    const uint32_t requests[] = {terminal::TCSETSF, terminal::TCSETSW2};
    const uint32_t gets[] = {terminal::TCGETS, terminal::TCGETS2};

    for (size_t i = 0; i < 2; i++) {
        ASSERT_EQ(request(f, page, gets[i]), fs::OK);
        ASSERT_EQ(write_now(f, 3), 3);
        serial::receive(g_port, g_bytes, 5);

        g_call.file = f;
        g_call.page = &page;
        g_call.cmd = requests[i];
        sched::task* setter = start_call(g_call, run_request, sched::TASK_FLAG_ELEVATED);
        ASSERT_NOT_NULL(setter);
        EXPECT_TRUE(blocks_before_deadline(setter));

        EXPECT_EQ(fs::ioctl(f, terminal::TCFLSH, terminal::TCIOFLUSH), fs::OK);
        EXPECT_TRUE(finish_call(g_call, setter));
        EXPECT_EQ(g_call.result, static_cast<ssize_t>(fs::OK));
        EXPECT_EQ(g_driver.cancels, static_cast<uint32_t>(i + 1));

        uint8_t byte = 0;
        EXPECT_EQ(fs::read(f, &byte, 1, fs::O_NONBLOCK), static_cast<ssize_t>(fs::ERR_AGAIN));
    }

    fs::close(f);
}

// A file that opens while the last close waits for the send is not a first open, and its send outlives that close
TEST(serial_port_writes, an_open_during_the_last_close_keeps_its_send) {
    fs::file* closing = open_port();
    ASSERT_NOT_NULL(closing);
    ASSERT_EQ(write_now(closing, 3), 3);

    g_call.file = closing;
    sched::task* closer = start_call(g_call, run_close, sched::TASK_FLAG_ELEVATED);
    ASSERT_NOT_NULL(closer);
    EXPECT_TRUE(blocks_before_deadline(closer));

    fs::file* opening = open_port();
    ASSERT_NOT_NULL(opening);
    EXPECT_EQ(g_driver.set_line_calls, 1u);

    g_driver.end_send(3);
    EXPECT_TRUE(finish_call(g_call, closer));
    EXPECT_EQ(write_now(opening, 3), 3);
    EXPECT_TRUE(g_driver.is_sending);
    EXPECT_EQ(g_driver.cancels, 0u);

    g_driver.end_send(3);
    fs::close(opening);
}
