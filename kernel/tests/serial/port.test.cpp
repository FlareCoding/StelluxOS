#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/serial/serial.h"
#include "drivers/serial/port.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sync/poll.h"
#include "syscall/handlers/sys_error_map.h"
#include "terminal/tty.h"
#include "tests/serial/fake_port_driver.h"

TEST_SUITE(serial_port);

using test_helpers::spin_wait;
using test_helpers::user_page;
using test_helpers::user_space_scope;

// The driver of every port the tests register, whose chip answers at once
static serial_test::fake_port_driver g_driver;

// A board no other test names
static constexpr serial::identity BOARD = {
    .chip = "testchip",
    .serial_number = "SN0042",
    .location = "9.1",
    .channel = -1,
};

static constexpr const char* BOARD_PATH = "/dev/serial/testchip-0042";

// A second board with the same serial number at another location, whose name takes more of the serial number
static constexpr serial::identity SECOND_BOARD = {
    .chip = "testchip",
    .serial_number = "SN0042",
    .location = "9.2",
    .channel = -1,
};

static constexpr const char* SECOND_BOARD_PATH = "/dev/serial/testchip-sn0042";

static constexpr size_t READ_BUFFER_BYTES = 64;

// A byte pattern whose period does not divide the 256 bytes a read copies out at once
static constexpr size_t PATTERN_PERIOD = 251;

// The ports a test has not unplugged yet, which the test's AFTER_EACH unplugs when an ASSERT ends it early
struct test_port {
    serial::port* port;
    bool          is_detached;
};

static constexpr size_t MAX_TEST_PORTS = 4;
static test_port g_test_ports[MAX_TEST_PORTS];

static test_port* find_test_port(serial::port* p) {
    for (test_port& tracked : g_test_ports) {
        if (tracked.port == p) {
            return &tracked;
        }
    }

    return nullptr;
}

static serial::port* plug(const serial::identity& id) {
    serial::port* p = serial::register_port(&g_driver, id);
    test_port* free_slot = find_test_port(nullptr);
    if (p && free_slot) {
        *free_slot = {.port = p, .is_detached = false};
    }

    return p;
}

static void detach_port(serial::port* p) {
    serial::detach(p);
    test_port* tracked = find_test_port(p);
    if (tracked) {
        tracked->is_detached = true;
    }
}

static void unregister_detached_port(serial::port* p) {
    serial::unregister_port(p);
    test_port* tracked = find_test_port(p);
    if (tracked) {
        *tracked = {};
    }
}

// What a driver does when its device leaves
static void unplug(serial::port* p) {
    detach_port(p);
    unregister_detached_port(p);
}

static int32_t unplug_leftover_ports() {
    for (test_port& tracked : g_test_ports) {
        if (tracked.port && !tracked.is_detached) {
            detach_port(tracked.port);
        }

        if (tracked.port) {
            unregister_detached_port(tracked.port);
        }
    }

    return 0;
}

AFTER_EACH(serial_port, unplug_leftover_ports);

static bool is_listed(const char* path) {
    fs::vattr attr = {};
    return fs::stat(path, &attr) == fs::OK;
}

static void receive_text(serial::port* p, const char* text) {
    serial::receive(p, reinterpret_cast<const uint8_t*>(text), string::strlen(text));
}

static void fill_pattern(uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bytes[i] = static_cast<uint8_t>(i % PATTERN_PERIOD);
    }
}

// Reads without waiting, since a wait on the test runner's task never wakes
static ssize_t read_now(fs::file* f, void* buf, size_t count) {
    return fs::read(f, buf, count, fs::O_NONBLOCK);
}

static uint32_t poll_events(fs::file* f) {
    uint32_t events = 0;
    RUN_ELEVATED(events = f->get_node()->poll(f, nullptr));
    return events;
}

TEST(serial_port, a_registered_port_is_listed_until_it_is_detached) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    EXPECT_TRUE(is_listed(BOARD_PATH));

    detach_port(p);
    EXPECT_FALSE(is_listed(BOARD_PATH));

    unregister_detached_port(p);
}

TEST(serial_port, a_second_board_with_the_same_serial_number_takes_a_longer_name) {
    serial::port* first = plug(BOARD);
    serial::port* second = plug(SECOND_BOARD);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    EXPECT_TRUE(is_listed(BOARD_PATH));
    EXPECT_TRUE(is_listed(SECOND_BOARD_PATH));

    unplug(second);
    unplug(first);
}

TEST(serial_port, a_port_whose_name_is_held_is_refused) {
    serial::port* first = plug(BOARD);
    ASSERT_NOT_NULL(first);

    // The same board at the same location again, as from a device that reports one port twice
    EXPECT_NULL(plug(BOARD));
    EXPECT_TRUE(is_listed(BOARD_PATH));

    unplug(first);
    EXPECT_FALSE(is_listed(BOARD_PATH));
}

TEST(serial_port, a_port_keeps_a_copy_of_its_identity) {
    char serial_number[] = "SN0042";
    serial::identity board = BOARD;
    board.serial_number = serial_number;

    serial::port* first = plug(board);
    ASSERT_NOT_NULL(first);

    // Were the port to keep the caller's string, the second board would read as another port of the first
    serial_number[0] = 'X';
    serial::port* second = plug(board);
    ASSERT_NOT_NULL(second);
    EXPECT_TRUE(is_listed(BOARD_PATH));
    EXPECT_TRUE(is_listed("/dev/serial/testchip-xn0042"));

    unplug(second);
    unplug(first);
}

TEST(serial_port, bytes_arriving_while_no_file_is_open_are_dropped) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    receive_text(p, "early");

    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    char buf[READ_BUFFER_BYTES] = {};
    EXPECT_EQ(read_now(f, buf, sizeof(buf)), static_cast<ssize_t>(fs::ERR_AGAIN));
    EXPECT_EQ(poll_events(f), sync::POLL_OUT);

    receive_text(p, "hello");
    EXPECT_EQ(poll_events(f), sync::POLL_IN | sync::POLL_OUT);
    ASSERT_EQ(read_now(f, buf, sizeof(buf)), 5);
    EXPECT_EQ(string::memcmp(buf, "hello", 5), 0);

    fs::close(f);
    unplug(p);
}

TEST(serial_port, the_last_close_drops_the_queued_bytes) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);

    fs::file* first = fs::open(BOARD_PATH, fs::O_RDONLY);
    fs::file* second = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    receive_text(p, "kept");
    fs::close(first);

    char buf[READ_BUFFER_BYTES] = {};
    ASSERT_EQ(read_now(second, buf, sizeof(buf)), 4);
    EXPECT_EQ(string::memcmp(buf, "kept", 4), 0);

    receive_text(p, "stale");
    fs::close(second);

    fs::file* reopened = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(reopened);
    EXPECT_EQ(read_now(reopened, buf, sizeof(buf)), static_cast<ssize_t>(fs::ERR_AGAIN));

    fs::close(reopened);
    unplug(p);
}

TEST(serial_port, a_full_queue_drops_what_does_not_fit) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    size_t sent_bytes = serial::RECEIVE_QUEUE_BYTES + READ_BUFFER_BYTES;
    auto* bytes = static_cast<uint8_t*>(heap::ualloc(sent_bytes));
    auto* received = static_cast<uint8_t*>(heap::ualloc(sent_bytes));
    ASSERT_NOT_NULL(bytes);
    ASSERT_NOT_NULL(received);
    fill_pattern(bytes, sent_bytes);

    serial::receive(p, bytes, sent_bytes);

    // One read takes the whole queue, many times what a read copies out at once
    char buf[READ_BUFFER_BYTES] = {};
    EXPECT_EQ(read_now(f, received, sent_bytes), static_cast<ssize_t>(serial::RECEIVE_QUEUE_BYTES));
    EXPECT_EQ(string::memcmp(received, bytes, serial::RECEIVE_QUEUE_BYTES), 0);
    EXPECT_EQ(read_now(f, buf, sizeof(buf)), static_cast<ssize_t>(fs::ERR_AGAIN));

    heap::ufree(received);
    heap::ufree(bytes);
    fs::close(f);
    unplug(p);
}

// The bytes kept queued across the queue's end, and the bytes received after them
static constexpr size_t KEPT_BYTES = 30;
static constexpr size_t WRAPPED_BYTES = 40;

TEST(serial_port, queued_bytes_survive_a_partial_read_and_the_wrap) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    size_t filler_bytes = serial::RECEIVE_QUEUE_BYTES - 10;
    auto* filler = static_cast<uint8_t*>(heap::ualloc(filler_bytes));
    ASSERT_NOT_NULL(filler);
    fill_pattern(filler, filler_bytes);

    // Reads all but the last bytes of a filling 10 bytes short of the end, so the next receive wraps. The read lands
    // in the filler's start, leaving the kept bytes' copy at its end untouched
    serial::receive(p, filler, filler_bytes);
    size_t drained_bytes = filler_bytes - KEPT_BYTES;
    EXPECT_EQ(read_now(f, filler, drained_bytes), static_cast<ssize_t>(drained_bytes));

    uint8_t wrapped[WRAPPED_BYTES];
    for (size_t i = 0; i < WRAPPED_BYTES; i++) {
        wrapped[i] = static_cast<uint8_t>('A' + i % 26);
    }

    serial::receive(p, wrapped, sizeof(wrapped));
    uint8_t out[KEPT_BYTES + WRAPPED_BYTES] = {};
    ASSERT_EQ(read_now(f, out, sizeof(out)), static_cast<ssize_t>(sizeof(out)));
    EXPECT_EQ(string::memcmp(out, filler + drained_bytes, KEPT_BYTES), 0);
    EXPECT_EQ(string::memcmp(out + KEPT_BYTES, wrapped, WRAPPED_BYTES), 0);

    // The next bytes come from the queue's start, where the head went past its end
    receive_text(p, "after");
    ASSERT_EQ(read_now(f, out, sizeof(out)), 5);
    EXPECT_EQ(string::memcmp(out, "after", 5), 0);

    heap::ufree(filler);
    fs::close(f);
    unplug(p);
}

// More than the 256 bytes a read copies out at once
static constexpr size_t USER_READ_BYTES = 300;
static constexpr size_t USER_PAGE_TAIL_BYTES = 256;

TEST(serial_port, a_read_into_a_program_stops_where_its_buffer_faults) {
    user_page page;
    ASSERT_NOT_NULL(page.bytes);
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    uint8_t bytes[USER_READ_BYTES];
    fill_pattern(bytes, sizeof(bytes));
    serial::receive(p, bytes, sizeof(bytes));

    // The first 256 bytes fill the page's end, and the next ones land past it, where the copy faults
    ssize_t n = 0;
    {
        user_space_scope scope(page.ctx);
        void* tail = reinterpret_cast<void*>(page.addr + pmm::PAGE_SIZE - USER_PAGE_TAIL_BYTES);
        n = fs::read_to_user(f, tail, sizeof(bytes), -1, fs::O_NONBLOCK);
    }

    EXPECT_EQ(n, static_cast<ssize_t>(USER_PAGE_TAIL_BYTES));
    EXPECT_EQ(string::memcmp(page.bytes + pmm::PAGE_SIZE - USER_PAGE_TAIL_BYTES, bytes, USER_PAGE_TAIL_BYTES), 0);

    fs::close(f);
    unplug(p);
}

TEST(serial_port, a_read_at_a_position_is_refused_and_takes_nothing) {
    user_page page;
    ASSERT_NOT_NULL(page.bytes);
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    receive_text(p, "kept");
    ssize_t n = 0;
    {
        user_space_scope scope(page.ctx);
        n = fs::read_to_user(f, reinterpret_cast<void*>(page.addr), READ_BUFFER_BYTES, 0, fs::O_NONBLOCK);
    }

    EXPECT_EQ(n, static_cast<ssize_t>(fs::ERR_NOSYS));
    char buf[READ_BUFFER_BYTES] = {};
    ASSERT_EQ(read_now(f, buf, sizeof(buf)), 4);
    EXPECT_EQ(string::memcmp(buf, "kept", 4), 0);

    fs::close(f);
    unplug(p);
}

// Asks FIONREAD, whose answer the page's first bytes take, as a program's ioctl does
static int32_t ask_queued_bytes(fs::file* f, user_page& page, int32_t* queued) {
    int32_t rc = fs::OK;
    {
        user_space_scope scope(page.ctx);
        rc = fs::ioctl(f, terminal::FIONREAD, page.addr);
    }

    *queued = *page.at<int32_t>(0);
    return rc;
}

TEST(serial_port, fionread_counts_the_queued_bytes_also_after_an_unplug) {
    user_page page;
    ASSERT_NOT_NULL(page.bytes);
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    int32_t queued = -1;
    receive_text(p, "hello");
    EXPECT_EQ(ask_queued_bytes(f, page, &queued), fs::OK);
    EXPECT_EQ(queued, 5);

    detach_port(p);
    EXPECT_EQ(ask_queued_bytes(f, page, &queued), fs::OK);
    EXPECT_EQ(queued, 5);

    char buf[READ_BUFFER_BYTES] = {};
    ASSERT_EQ(read_now(f, buf, sizeof(buf)), 5);
    EXPECT_EQ(ask_queued_bytes(f, page, &queued), fs::OK);
    EXPECT_EQ(queued, 0);

    // Every other request fails once the device has left
    EXPECT_EQ(fs::ioctl(f, terminal::TCGETS, 0), fs::ERR_IO);

    fs::close(f);
    unregister_detached_port(p);
}

// A blocking read on a task of its own, since the test runner's task cannot sleep
static fs::file* g_reader_file;
static char g_reader_buf[READ_BUFFER_BYTES];
static sync::atomic<int64_t> g_reader_result;
static sync::atomic<uint32_t> g_reader_done;

static void read_and_exit(void*) {
    g_reader_result.store_relaxed(fs::read(g_reader_file, g_reader_buf, sizeof(g_reader_buf)));
    g_reader_done.store_release(1);
    sched::exit(0);
}

static sched::task* start_blocked_reader(fs::file* f) {
    g_reader_file = f;
    g_reader_done.store_relaxed(0);

    sched::task* reader = test_helpers::start_pinned_task(read_and_exit, nullptr, "serial_reader");
    if (reader && !test_helpers::blocks_before_deadline(reader)) {
        test_helpers::unpin(reader);
        return nullptr;
    }

    return reader;
}

TEST(serial_port, a_blocking_read_returns_once_a_byte_arrives) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    sched::task* reader = start_blocked_reader(f);
    ASSERT_NOT_NULL(reader);
    EXPECT_EQ(g_reader_done.load_acquire(), 0u);

    receive_text(p, "x");
    ASSERT_TRUE(spin_wait(g_reader_done));
    EXPECT_EQ(g_reader_result.load_acquire(), 1);
    EXPECT_EQ(g_reader_buf[0], 'x');

    test_helpers::unpin(reader);
    fs::close(f);
    unplug(p);
}

TEST(serial_port, a_kill_ends_a_blocking_read) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    sched::task* reader = start_blocked_reader(f);
    ASSERT_NOT_NULL(reader);

    RUN_ELEVATED(sched::force_wake_for_kill(reader));
    ASSERT_TRUE(spin_wait(g_reader_done));
    EXPECT_EQ(g_reader_result.load_acquire(), static_cast<int64_t>(fs::ERR_INTR));

    // The read syscall restarts after the signal's handler, or fails with EINTR
    EXPECT_EQ(syscall::error_map::map_fs_error(fs::ERR_INTR), syscall::ERESTARTSYS);

    test_helpers::unpin(reader);
    fs::close(f);
    unplug(p);
}

TEST(serial_port, an_unplug_wakes_a_blocking_read_with_end_of_file) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    sched::task* reader = start_blocked_reader(f);
    ASSERT_NOT_NULL(reader);

    detach_port(p);
    ASSERT_TRUE(spin_wait(g_reader_done));
    EXPECT_EQ(g_reader_result.load_acquire(), 0);

    test_helpers::unpin(reader);
    unregister_detached_port(p);
    fs::close(f);
}

TEST(serial_port, an_unplug_frees_the_name_at_once_while_open_files_read_what_was_queued) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    receive_text(p, "queued");
    detach_port(p);

    // Bytes a stream delivers after the unplug are dropped, and the port opens no new file
    receive_text(p, "late");
    int32_t open_result = fs::OK;
    RUN_ELEVATED(open_result = f->get_node()->open(f, fs::O_RDONLY));
    EXPECT_EQ(open_result, fs::ERR_IO);

    // A board coming back takes the name before the unplugged port's driver lets it go
    serial::port* back = plug(BOARD);
    ASSERT_NOT_NULL(back);
    EXPECT_TRUE(is_listed(BOARD_PATH));
    EXPECT_FALSE(is_listed(SECOND_BOARD_PATH));

    // The unplugged port stays for its open file
    char buf[READ_BUFFER_BYTES] = {};
    EXPECT_EQ(poll_events(f), sync::POLL_IN | sync::POLL_OUT | sync::POLL_HUP);
    ASSERT_EQ(read_now(f, buf, sizeof(buf)), 6);
    EXPECT_EQ(string::memcmp(buf, "queued", 6), 0);
    EXPECT_EQ(read_now(f, buf, sizeof(buf)), 0);
    EXPECT_EQ(poll_events(f), sync::POLL_OUT | sync::POLL_HUP);

    unregister_detached_port(p);
    fs::close(f);
    unplug(back);
}

TEST(serial_port, a_failed_port_reads_what_was_queued_then_fails) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    receive_text(p, "last");
    serial::receive_failed(p);

    // Readable rather than in error, since only reads fail
    char buf[READ_BUFFER_BYTES] = {};
    EXPECT_EQ(poll_events(f), sync::POLL_IN | sync::POLL_OUT);
    ASSERT_EQ(read_now(f, buf, sizeof(buf)), 4);
    EXPECT_EQ(poll_events(f), sync::POLL_IN | sync::POLL_OUT);
    EXPECT_EQ(read_now(f, buf, sizeof(buf)), static_cast<ssize_t>(fs::ERR_IO));

    // An unplug then turns the failure into end of file, so a poll reports the hang-up without POLL_IN
    detach_port(p);
    EXPECT_EQ(poll_events(f), sync::POLL_OUT | sync::POLL_HUP);
    EXPECT_EQ(read_now(f, buf, sizeof(buf)), 0);

    unregister_detached_port(p);
    fs::close(f);
}

TEST(serial_port, a_failed_port_unplugged_with_bytes_queued_is_readable_until_they_are_read) {
    serial::port* p = plug(BOARD);
    ASSERT_NOT_NULL(p);
    fs::file* f = fs::open(BOARD_PATH, fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    receive_text(p, "last");
    serial::receive_failed(p);
    detach_port(p);

    char buf[READ_BUFFER_BYTES] = {};
    EXPECT_EQ(poll_events(f), sync::POLL_IN | sync::POLL_OUT | sync::POLL_HUP);
    ASSERT_EQ(read_now(f, buf, sizeof(buf)), 4);
    EXPECT_EQ(poll_events(f), sync::POLL_OUT | sync::POLL_HUP);
    EXPECT_EQ(read_now(f, buf, sizeof(buf)), 0);

    unregister_detached_port(p);
    fs::close(f);
}
