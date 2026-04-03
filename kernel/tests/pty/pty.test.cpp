#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "pty/pty.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "terminal/terminal.h"
#include "terminal/line_discipline.h"
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
