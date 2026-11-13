#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "pty/pty.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "terminal/terminal.h"
#include "terminal/line_discipline.h"
#include "signals/signal_types.h"
#include "common/ring_buffer.h"
#include "fs/fstypes.h"
#include "sync/poll.h"
#include "common/string.h"

TEST_SUITE(pty_test);

TEST(pty_test, create_pair_succeeds) {
    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);
    ASSERT_NOT_NULL(master);
    ASSERT_NOT_NULL(slave);
    EXPECT_EQ(master->type, resource::resource_type::PTY);
    EXPECT_EQ(slave->type, resource::resource_type::PTY);

    resource::resource_release(master);
    resource::resource_release(slave);
}

TEST(pty_test, master_to_slave_write_read) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    // Set raw mode so bytes pass through directly
    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);

    const char msg[] = "hello";
    ASSERT_EQ(resource::write(task, hm, msg, 5), static_cast<ssize_t>(5));

    char buf[16] = {};
    ASSERT_EQ(resource::read(task, hs, buf, 16), static_cast<ssize_t>(5));
    EXPECT_STREQ(buf, "hello");

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, slave_to_master_write_read) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    const char msg[] = "world";
    ASSERT_EQ(resource::write(task, hs, msg, 5), static_cast<ssize_t>(5));

    char buf[16] = {};
    ASSERT_EQ(resource::read(task, hm, buf, 16), static_cast<ssize_t>(5));
    EXPECT_STREQ(buf, "world");

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, close_master_slave_reads_eof) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    EXPECT_EQ(resource::close(task, hm), resource::OK);

    char buf[16] = {};
    EXPECT_EQ(resource::read(task, hs, buf, 16), static_cast<ssize_t>(0));

    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, close_slave_master_reads_eof) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    EXPECT_EQ(resource::close(task, hs), resource::OK);

    char buf[16] = {};
    EXPECT_EQ(resource::read(task, hm, buf, 16), static_cast<ssize_t>(0));

    EXPECT_EQ(resource::close(task, hm), resource::OK);
}

TEST(pty_test, close_slave_master_write_epipe) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    // Set raw mode for simpler write semantics
    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);

    EXPECT_EQ(resource::close(task, hs), resource::OK);

    EXPECT_EQ(resource::write(task, hm, "x", 1), static_cast<ssize_t>(resource::ERR_PIPE));

    EXPECT_EQ(resource::close(task, hm), resource::OK);
}

TEST(pty_test, raw_mode_no_echo) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);

    // Write to master (raw mode: no echo, bytes pass to slave input)
    ASSERT_EQ(resource::write(task, hm, "abc", 3), static_cast<ssize_t>(3));

    // Set master to non-blocking so we can check for no echo without hanging
    resource::set_status_flags(master, fs::O_NONBLOCK);

    // Master read should have nothing (no echo in raw mode)
    char echo_buf[16] = {};
    ssize_t echo_rc = resource::read(task, hm, echo_buf, 16);
    EXPECT_EQ(echo_rc, static_cast<ssize_t>(resource::ERR_AGAIN));

    // Slave read should have the bytes
    char buf[16] = {};
    ASSERT_EQ(resource::read(task, hs, buf, 16), static_cast<ssize_t>(3));
    EXPECT_STREQ(buf, "abc");

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

// Captures the last signal an ISIG interception delivered
static uint32_t g_isig_caught;

static void isig_capture(void*, uint32_t sig) {
    g_isig_caught = sig;
}

TEST(pty_test, isig_intercepts_interrupt_byte) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    ep->channel->m_sig = { isig_capture, nullptr };
    g_isig_caught = 0;

    // ^C mid-line: the pending line is flushed and the byte swallowed
    ASSERT_EQ(resource::write(task, hm, "ab\x03", 3), static_cast<ssize_t>(3));
    EXPECT_EQ(g_isig_caught, signals::SIGINT);

    // The echo carries "ab" then the "^C" caret sequence
    char echo_buf[16] = {};
    ASSERT_EQ(resource::read(task, hm, echo_buf, 16), static_cast<ssize_t>(6));
    EXPECT_STREQ(echo_buf, "ab^C\r\n");

    // Only the next complete line reaches the slave
    ASSERT_EQ(resource::write(task, hm, "cd\r", 3), static_cast<ssize_t>(3));
    char buf[16] = {};
    ASSERT_EQ(resource::read(task, hs, buf, 16), static_cast<ssize_t>(3));
    EXPECT_STREQ(buf, "cd\n");

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, isig_quit_and_susp_bytes) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    ep->channel->m_sig = { isig_capture, nullptr };

    g_isig_caught = 0;
    ASSERT_EQ(resource::write(task, hm, "\x1C", 1), static_cast<ssize_t>(1));
    EXPECT_EQ(g_isig_caught, signals::SIGQUIT);

    g_isig_caught = 0;
    ASSERT_EQ(resource::write(task, hm, "\x1A", 1), static_cast<ssize_t>(1));
    EXPECT_EQ(g_isig_caught, signals::SIGTSTP);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, isig_raw_mode_passes_bytes) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    ep->channel->m_sig = { isig_capture, nullptr };

    // The raw shortcut clears ISIG, control bytes are plain input
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);
    g_isig_caught = 0;
    ASSERT_EQ(resource::write(task, hm, "\x03", 1), static_cast<ssize_t>(1));
    EXPECT_EQ(g_isig_caught, 0U);

    char buf[4] = {};
    ASSERT_EQ(resource::read(task, hs, buf, 4), static_cast<ssize_t>(1));
    EXPECT_EQ(buf[0], '\x03');

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, isig_reenabled_in_raw_mode) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    ep->channel->m_sig = { isig_capture, nullptr };

    // TCSETS can turn ISIG back on independently of raw mode
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);
    terminal::ld_set_isig(&ep->channel->m_ld, true);

    g_isig_caught = 0;
    ASSERT_EQ(resource::write(task, hm, "x\x03y", 3), static_cast<ssize_t>(3));
    EXPECT_EQ(g_isig_caught, signals::SIGINT);

    // The surrounding bytes still pass through, the ^C does not
    char buf[4] = {};
    ASSERT_EQ(resource::read(task, hs, buf, 4), static_cast<ssize_t>(2));
    EXPECT_EQ(buf[0], 'x');
    EXPECT_EQ(buf[1], 'y');

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, raw_master_write_stops_where_the_slave_fills) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);
    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    static char sent[3 * pty::PTY_RING_CAPACITY];
    for (size_t i = 0; i < sizeof(sent); i++) {
        sent[i] = static_cast<char>('a' + i % 26);
    }

    // The slave takes what its buffer holds, the rest stays with the writer
    const ssize_t room = static_cast<ssize_t>(ep->channel->m_input_rb->capacity - 1);
    const size_t rest = sizeof(sent) - static_cast<size_t>(room);
    EXPECT_EQ(resource::write(task, hm, sent, sizeof(sent)), room);
    EXPECT_EQ(resource::write(task, hm, sent + room, rest), static_cast<ssize_t>(resource::ERR_AGAIN));
    EXPECT_EQ(master->ops->poll(master, nullptr) & sync::POLL_OUT, 0U);

    static char received[sizeof(sent)];
    EXPECT_EQ(resource::read(task, hs, received, 1000), static_cast<ssize_t>(1000));
    EXPECT_EQ(master->ops->poll(master, nullptr) & sync::POLL_OUT, sync::POLL_OUT);
    EXPECT_EQ(resource::write(task, hm, sent + room, rest), static_cast<ssize_t>(1000));

    ssize_t got = 1000;
    ssize_t expected = room + 1000;
    while (got < expected) {
        ssize_t n = resource::read(task, hs, received + got, sizeof(received) - static_cast<size_t>(got));
        if (n <= 0) {
            break;
        }

        got += n;
    }

    bool is_intact = got == expected;
    for (ssize_t i = 0; is_intact && i < expected; i++) {
        is_intact = received[i] == sent[i];
    }

    EXPECT_TRUE(is_intact);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_line_end_waits_for_room_for_its_line) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // Lines of 100 bytes and a newline until fewer than 101 bytes of room are left
    static char line[101];
    for (size_t i = 0; i < 100; i++) {
        line[i] = 'x';
    }

    line[100] = '\r';
    const size_t lines = (ep->channel->m_input_rb->capacity - 1) / sizeof(line);
    for (size_t i = 0; i < lines; i++) {
        EXPECT_EQ(resource::write(task, hm, line, sizeof(line)), static_cast<ssize_t>(sizeof(line)));
    }

    // The letters wait in the line, its end until the whole line fits
    for (size_t i = 0; i < 100; i++) {
        line[i] = 'y';
    }

    EXPECT_EQ(resource::write(task, hm, line, sizeof(line)), static_cast<ssize_t>(100));
    EXPECT_EQ(resource::write(task, hm, "\r", 1), static_cast<ssize_t>(resource::ERR_AGAIN));
    EXPECT_EQ(master->ops->poll(master, nullptr) & sync::POLL_OUT, 0U);

    static char received[101];
    EXPECT_EQ(resource::read(task, hs, received, sizeof(received)), static_cast<ssize_t>(sizeof(received)));
    EXPECT_EQ(master->ops->poll(master, nullptr) & sync::POLL_OUT, sync::POLL_OUT);
    EXPECT_EQ(resource::write(task, hm, "\r", 1), static_cast<ssize_t>(1));

    for (size_t i = 1; i < lines; i++) {
        EXPECT_EQ(resource::read(task, hs, received, sizeof(received)), static_cast<ssize_t>(sizeof(received)));
        EXPECT_EQ(received[0], 'x');
    }

    EXPECT_EQ(resource::read(task, hs, received, sizeof(received)), static_cast<ssize_t>(sizeof(received)));
    EXPECT_EQ(received[0], 'y');
    EXPECT_EQ(received[99], 'y');
    EXPECT_EQ(received[100], '\n');

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_line_keeps_tabs_and_utf8) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // A tab, an e with an acute accent in two UTF-8 bytes, and a control byte the line drops
    EXPECT_EQ(resource::write(task, hm, "a\tb\xc3\xa9\x01\r", 7), static_cast<ssize_t>(7));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(6));
    EXPECT_EQ(string::memcmp(line, "a\tb\xc3\xa9\n", 6), 0);

    char echo_buf[16] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(7));
    EXPECT_EQ(string::memcmp(echo_buf, "a\tb\xc3\xa9\r\n", 7), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_erase_takes_a_whole_character) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // Erasing the two-byte accented e takes both of its bytes and rubs out one column
    EXPECT_EQ(resource::write(task, hm, "x\xc3\xa9\x7f\r", 5), static_cast<ssize_t>(5));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(line, "x\n", 2), 0);

    char echo_buf[16] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(8));
    EXPECT_EQ(string::memcmp(echo_buf, "x\xc3\xa9\b \b\r\n", 8), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_erase_backs_over_a_tab) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // The tab after "ab" reaches column 8, so erasing it backs over six columns
    EXPECT_EQ(resource::write(task, hm, "ab\t\x7f\r", 5), static_cast<ssize_t>(5));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(line, "ab\n", 3), 0);

    char echo_buf[16] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(11));
    EXPECT_EQ(string::memcmp(echo_buf, "ab\t\b\b\b\b\b\b\r\n", 11), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_word_erase_takes_the_blanks_and_the_word_before_them) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // Ctrl W takes the space and the four-column word with an accented e before it, rubbing out five columns
    EXPECT_EQ(resource::write(task, hm, "ls caf\xc3\xa9 \x17\r", 11), static_cast<ssize_t>(11));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(line, "ls \n", 4), 0);

    char echo_buf[32] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(26));
    EXPECT_EQ(string::memcmp(echo_buf, "ls caf\xc3\xa9 \b \b\b \b\b \b\b \b\b \b\r\n", 26), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_word_erase_rubs_out_every_column_of_a_tab) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // The tab after "ab" reaches column 8, so erasing it and "ab" rubs out eight columns
    EXPECT_EQ(resource::write(task, hm, "ab\t\x17\r", 5), static_cast<ssize_t>(5));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(1));
    EXPECT_EQ(string::memcmp(line, "\n", 1), 0);

    char echo_buf[32] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(29));
    EXPECT_EQ(string::memcmp(echo_buf, "ab\t\b \b\b \b\b \b\b \b\b \b\b \b\b \b\b \b\r\n", 29), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_word_erase_on_an_empty_line_echoes_nothing) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    EXPECT_EQ(resource::write(task, hm, "\x17x\r", 3), static_cast<ssize_t>(3));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(line, "x\n", 2), 0);

    char echo_buf[16] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(echo_buf, "x\r\n", 3), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_line_keeps_4095_bytes_and_rings_past_them) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // Five letters past what a line holds drop, each ringing the bell
    static char sent[terminal::LD_LINE_BUF_MAX + 6];
    for (size_t i = 0; i + 1 < sizeof(sent); i++) {
        sent[i] = 'a';
    }

    sent[sizeof(sent) - 1] = '\r';
    EXPECT_EQ(resource::write(task, hm, sent, sizeof(sent)), static_cast<ssize_t>(sizeof(sent)));

    static char line[sizeof(sent)];
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(terminal::LD_LINE_BUF_MAX + 1));
    EXPECT_EQ(line[terminal::LD_LINE_BUF_MAX - 1], 'a');
    EXPECT_EQ(line[terminal::LD_LINE_BUF_MAX], '\n');

    static char echo_buf[sizeof(sent) + 8];
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)),
              static_cast<ssize_t>(terminal::LD_LINE_BUF_MAX + 7));
    EXPECT_EQ(echo_buf[terminal::LD_LINE_BUF_MAX - 1], 'a');
    EXPECT_EQ(string::memcmp(echo_buf + terminal::LD_LINE_BUF_MAX, "\a\a\a\a\a\r\n", 7), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_eof_on_an_empty_line_reads_as_end_of_file) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // Ctrl D on an empty line polls readable, reads as 0 once and echoes nothing
    EXPECT_EQ(resource::write(task, hm, "\x04", 1), static_cast<ssize_t>(1));
    EXPECT_EQ(slave->ops->poll(slave, nullptr) & sync::POLL_IN, sync::POLL_IN);

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(0));
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(resource::ERR_AGAIN));

    EXPECT_EQ(resource::write(task, hm, "z\r", 2), static_cast<ssize_t>(2));
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(line, "z\n", 2), 0);

    char echo_buf[16] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(echo_buf, "z\r\n", 3), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_eof_sends_a_pending_line_without_its_end) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // Ctrl D after "abc" hands over the three letters, with no line end and no end of file
    EXPECT_EQ(resource::write(task, hm, "abc\x04", 4), static_cast<ssize_t>(4));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(line, "abc", 3), 0);
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(resource::ERR_AGAIN));

    char echo_buf[16] = {};
    EXPECT_EQ(resource::read(task, hm, echo_buf, sizeof(echo_buf)), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(echo_buf, "abc", 3), 0);

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, cooked_eof_reads_after_the_input_before_it) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // A line, an end of file and a line in one write reach the reader in that order, the end of file
    // keeping the slave readable once the line before it is read
    EXPECT_EQ(resource::write(task, hm, "one\r\x04two\r", 9), static_cast<ssize_t>(9));

    char line[16] = {};
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(line, "one\n", 4), 0);
    EXPECT_EQ(slave->ops->poll(slave, nullptr) & sync::POLL_IN, sync::POLL_IN);
    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(0));

    EXPECT_EQ(resource::read(task, hs, line, sizeof(line)), static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(line, "two\n", 4), 0);

    // An end of file no one reads goes away with the pty
    EXPECT_EQ(resource::write(task, hm, "\x04", 1), static_cast<ssize_t>(1));

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}

TEST(pty_test, raw_read_skips_an_eof_left_from_cooked_mode) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* master = nullptr;
    resource::resource_object* slave = nullptr;
    ASSERT_EQ(pty::create_pair(&master, &slave), resource::OK);

    resource::handle_t hm = -1;
    resource::handle_t hs = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    resource::set_status_flags(master, fs::O_NONBLOCK);
    resource::set_status_flags(slave, fs::O_NONBLOCK);

    // An end of file still queued when the program goes raw is skipped, never read as the end
    EXPECT_EQ(resource::write(task, hm, "\x04", 1), static_cast<ssize_t>(1));
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);
    EXPECT_EQ(resource::write(task, hm, "q", 1), static_cast<ssize_t>(1));

    char got[16] = {};
    EXPECT_EQ(resource::read(task, hs, got, sizeof(got)), static_cast<ssize_t>(1));
    EXPECT_EQ(got[0], 'q');

    EXPECT_EQ(resource::close(task, hm), resource::OK);
    EXPECT_EQ(resource::close(task, hs), resource::OK);
}
