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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hs), resource::HANDLE_OK);
    resource::resource_release(slave);

    auto* ep = static_cast<pty::pty_endpoint*>(slave->impl);
    terminal::ld_set_mode(&ep->channel->m_ld, terminal::STLX_TCSETS_RAW);

    // Write to master (raw mode: no echo, bytes pass to slave input)
    ASSERT_EQ(resource::write(task, hm, "abc", 3), static_cast<ssize_t>(3));

    // Set master to non-blocking so we can check for no echo without hanging
    ASSERT_EQ(resource::set_handle_flags(&task->handles, hm, fs::O_NONBLOCK), resource::HANDLE_OK);

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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, master, resource::resource_type::PTY,
              resource::RIGHT_READ | resource::RIGHT_WRITE, &hm), resource::HANDLE_OK);
    resource::resource_release(master);
    ASSERT_EQ(resource::alloc_handle(&task->handles, slave, resource::resource_type::PTY,
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
