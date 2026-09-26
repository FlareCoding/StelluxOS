#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "socket/unix_socket.h"
#include "resource/socket_ops.h"
#include "net/inet.h"
#include "dynpriv/dynpriv.h"
#include "common/ring_buffer.h"
#include "socket/listener.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"
#include "fs/fstypes.h"
#include "fs/socket_node.h"

TEST_SUITE(socket_test);

// Ring buffer tests

TEST(socket_test, ring_buffer_create_destroy) {
    auto* rb = ring_buffer_create(RING_BUFFER_DEFAULT_CAPACITY);
    ASSERT_NOT_NULL(rb);
    ASSERT_NOT_NULL(rb->data);
    EXPECT_GT(rb->capacity, RING_BUFFER_DEFAULT_CAPACITY);
    EXPECT_EQ(rb->head, 0u);
    EXPECT_EQ(rb->tail, 0u);
    EXPECT_FALSE(rb->writer_closed);
    EXPECT_FALSE(rb->reader_closed);
    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_write_read_basic) {
    auto* rb = ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);

    const uint8_t msg[] = "hello ring";
    ssize_t nw = ring_buffer_write(rb, msg, 10);
    EXPECT_EQ(nw, static_cast<ssize_t>(10));

    uint8_t buf[32] = {};
    ssize_t nr = ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(10));
    EXPECT_EQ(string::memcmp(buf, msg, 10), 0);

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_multiple_writes_single_read) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("aaa"), 3),
        static_cast<ssize_t>(3));
    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("bbb"), 3),
        static_cast<ssize_t>(3));
    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("ccc"), 3),
        static_cast<ssize_t>(3));

    uint8_t buf[32] = {};
    ssize_t nr = ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(9));
    EXPECT_EQ(string::memcmp(buf, "aaabbbccc", 9), 0);

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_short_read) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("xyz"), 3),
        static_cast<ssize_t>(3));

    uint8_t buf[1] = {};
    ssize_t nr = ring_buffer_read(rb, buf, 1);
    EXPECT_EQ(nr, static_cast<ssize_t>(1));
    EXPECT_EQ(buf[0], static_cast<uint8_t>('x'));

    nr = ring_buffer_read(rb, buf, 1);
    EXPECT_EQ(nr, static_cast<ssize_t>(1));
    EXPECT_EQ(buf[0], static_cast<uint8_t>('y'));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_eof_after_close_write) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("ab"), 2),
        static_cast<ssize_t>(2));
    ring_buffer_close_write(rb);

    uint8_t buf[32] = {};
    ssize_t nr = ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(2));

    nr = ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_epipe_after_close_read) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ring_buffer_close_read(rb);

    ssize_t nw = ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("x"), 1);
    EXPECT_EQ(nw, static_cast<ssize_t>(RB_ERR_PIPE));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_empty_returns_eagain) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    uint8_t buf[8] = {};
    ssize_t nr = ring_buffer_read(rb, buf, sizeof(buf), true);
    EXPECT_EQ(nr, static_cast<ssize_t>(RB_ERR_AGAIN));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_full_returns_eagain) {
    auto* rb = ring_buffer_create(16);
    ASSERT_NOT_NULL(rb);

    uint8_t fill[64];
    string::memset(fill, 'A', sizeof(fill));

    // Fill the buffer
    ssize_t nw = ring_buffer_write(rb, fill, sizeof(fill));
    EXPECT_GT(nw, static_cast<ssize_t>(0));

    // Now try non-blocking write when full
    nw = ring_buffer_write(rb, fill, 1, true);
    EXPECT_EQ(nw, static_cast<ssize_t>(RB_ERR_AGAIN));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_with_data_returns_data) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("test"), 4),
        static_cast<ssize_t>(4));

    uint8_t buf[32] = {};
    ssize_t nr = ring_buffer_read(rb, buf, sizeof(buf), true);
    EXPECT_EQ(nr, static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(buf, "test", 4), 0);

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_eof_returns_zero) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ring_buffer_close_write(rb);

    uint8_t buf[8] = {};
    ssize_t nr = ring_buffer_read(rb, buf, sizeof(buf), true);
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF, not EAGAIN

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_zero_length_returns_inval) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    uint8_t buf[1] = {};
    EXPECT_EQ(ring_buffer_read(rb, buf, 0), static_cast<ssize_t>(RB_ERR_INVAL));
    EXPECT_EQ(ring_buffer_write(rb, buf, 0), static_cast<ssize_t>(RB_ERR_INVAL));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_null_args_returns_inval) {
    auto* rb = ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    EXPECT_EQ(ring_buffer_read(nullptr, nullptr, 1), static_cast<ssize_t>(RB_ERR_INVAL));
    EXPECT_EQ(ring_buffer_read(rb, nullptr, 1), static_cast<ssize_t>(RB_ERR_INVAL));
    EXPECT_EQ(ring_buffer_write(nullptr, nullptr, 1), static_cast<ssize_t>(RB_ERR_INVAL));
    EXPECT_EQ(ring_buffer_write(rb, nullptr, 1), static_cast<ssize_t>(RB_ERR_INVAL));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_peek_leaves_the_bytes_queued) {
    auto* rb = ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);
    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("abcdef"), 6), static_cast<ssize_t>(6));

    uint8_t buf[8] = {};
    EXPECT_EQ(ring_buffer_peek(rb, buf, 4), static_cast<size_t>(4));
    EXPECT_EQ(string::memcmp(buf, "abcd", 4), 0);

    EXPECT_EQ(ring_buffer_read(rb, buf, sizeof(buf)), static_cast<ssize_t>(6));
    EXPECT_EQ(string::memcmp(buf, "abcdef", 6), 0);

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_peek_joins_bytes_that_wrap_around) {
    auto* rb = ring_buffer_create(8);
    ASSERT_NOT_NULL(rb);

    // Moving the read position near the end makes the next write wrap
    uint8_t fill[12] = {};
    ASSERT_EQ(ring_buffer_write(rb, fill, sizeof(fill)), static_cast<ssize_t>(sizeof(fill)));
    ASSERT_EQ(ring_buffer_read(rb, fill, sizeof(fill)), static_cast<ssize_t>(sizeof(fill)));
    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("abcdefgh"), 8), static_cast<ssize_t>(8));

    uint8_t buf[8] = {};
    EXPECT_EQ(ring_buffer_peek(rb, buf, sizeof(buf)), sizeof(buf));
    EXPECT_EQ(string::memcmp(buf, "abcdefgh", 8), 0);

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_skip_drops_bytes_and_frees_room) {
    auto* rb = ring_buffer_create(8);
    ASSERT_NOT_NULL(rb);

    uint8_t fill[15];
    for (size_t i = 0; i < sizeof(fill); i++) {
        fill[i] = static_cast<uint8_t>(i);
    }
    ASSERT_EQ(ring_buffer_write(rb, fill, sizeof(fill)), static_cast<ssize_t>(sizeof(fill)));
    EXPECT_EQ(ring_buffer_write(rb, fill, 1, true), static_cast<ssize_t>(RB_ERR_AGAIN));

    EXPECT_EQ(ring_buffer_skip(rb, 5), static_cast<size_t>(5));
    EXPECT_EQ(ring_buffer_write(rb, fill, 5, true), static_cast<ssize_t>(5));

    uint8_t first = 0;
    EXPECT_EQ(ring_buffer_read(rb, &first, 1), static_cast<ssize_t>(1));
    EXPECT_EQ(first, static_cast<uint8_t>(5));
    EXPECT_EQ(ring_buffer_skip(rb, 64), static_cast<size_t>(14));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_wait_readable_reports_what_is_queued) {
    auto* rb = ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);
    EXPECT_EQ(ring_buffer_wait_readable(rb, true), static_cast<ssize_t>(RB_ERR_AGAIN));

    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("xyz"), 3), static_cast<ssize_t>(3));
    EXPECT_EQ(ring_buffer_wait_readable(rb, false), static_cast<ssize_t>(3));

    uint8_t buf[4] = {};
    EXPECT_EQ(ring_buffer_read(rb, buf, sizeof(buf)), static_cast<ssize_t>(3));

    ring_buffer_close_write(rb);
    EXPECT_EQ(ring_buffer_wait_readable(rb, false), static_cast<ssize_t>(0));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_refuses_writes_once_its_writer_closes) {
    auto* rb = ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);

    ring_buffer_close_write(rb);
    EXPECT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("x"), 1), static_cast<ssize_t>(RB_ERR_PIPE));
    EXPECT_EQ(ring_buffer_write_all(rb, reinterpret_cast<const uint8_t*>("x"), 1), static_cast<ssize_t>(RB_ERR_PIPE));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_drains_then_ends_once_its_reader_closes) {
    auto* rb = ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("ab"), 2), static_cast<ssize_t>(2));
    ring_buffer_close_read(rb);

    uint8_t buf[4] = {};
    EXPECT_EQ(ring_buffer_read(rb, buf, sizeof(buf), true), static_cast<ssize_t>(2));
    EXPECT_EQ(ring_buffer_read(rb, buf, sizeof(buf), true), static_cast<ssize_t>(0));
    EXPECT_EQ(ring_buffer_wait_readable(rb, true), static_cast<ssize_t>(0));

    ring_buffer_destroy(rb);
}

// Socket pair creation and data flow

TEST(socket_test, create_socket_pair_succeeds) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    ASSERT_NOT_NULL(obj_a);
    ASSERT_NOT_NULL(obj_b);
    EXPECT_EQ(obj_a->type, resource::resource_type::SOCKET);
    EXPECT_EQ(obj_b->type, resource::resource_type::SOCKET);
    EXPECT_NOT_NULL(obj_a->ops);
    EXPECT_NOT_NULL(obj_b->ops);
    EXPECT_NOT_NULL(obj_a->impl);
    EXPECT_NOT_NULL(obj_b->impl);

    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, socketpair_write_read) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    const char* msg = "socket-hello";
    ASSERT_EQ(resource::write(task, h0, msg, 12), static_cast<ssize_t>(12));

    char buf[32] = {};
    ASSERT_EQ(resource::read(task, h1, buf, 32), static_cast<ssize_t>(12));
    EXPECT_STREQ(buf, "socket-hello");

    EXPECT_EQ(resource::close(task, h0), resource::OK);
    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

TEST(socket_test, socketpair_bidirectional) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    ASSERT_EQ(resource::write(task, h1, "world", 5), static_cast<ssize_t>(5));

    char buf[32] = {};
    ASSERT_EQ(resource::read(task, h0, buf, 32), static_cast<ssize_t>(5));
    EXPECT_STREQ(buf, "world");

    EXPECT_EQ(resource::close(task, h0), resource::OK);
    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

TEST(socket_test, socketpair_eof_on_peer_close) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    EXPECT_EQ(resource::close(task, h0), resource::OK);

    char buf[8] = {};
    ssize_t nr = resource::read(task, h1, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF

    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

TEST(socket_test, socketpair_epipe_on_peer_close) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    EXPECT_EQ(resource::close(task, h1), resource::OK);

    ssize_t nw = resource::write(task, h0, "x", 1);
    EXPECT_EQ(nw, static_cast<ssize_t>(resource::ERR_PIPE));

    EXPECT_EQ(resource::close(task, h0), resource::OK);
}

TEST(socket_test, socketpair_drain_then_eof) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    ASSERT_EQ(resource::write(task, h0, "abc", 3), static_cast<ssize_t>(3));
    EXPECT_EQ(resource::close(task, h0), resource::OK);

    char buf[8] = {};
    ssize_t nr = resource::read(task, h1, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(buf, "abc", 3), 0);

    nr = resource::read(task, h1, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF after drain

    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

// Unbound socket tests

TEST(socket_test, create_unbound_socket) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);
    ASSERT_NOT_NULL(obj);
    EXPECT_EQ(obj->type, resource::resource_type::SOCKET);

    auto* sock = static_cast<socket::unix_socket*>(obj->impl);
    ASSERT_NOT_NULL(sock);
    EXPECT_EQ(sock->state, socket::SOCK_STATE_UNBOUND);

    resource::resource_release(obj);
}

TEST(socket_test, unbound_socket_read_returns_notconn) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    char buf[8] = {};
    ssize_t nr = resource::read(task, h, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(resource::ERR_NOTCONN));

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

TEST(socket_test, unbound_socket_write_returns_notconn) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    ssize_t nw = resource::write(task, h, "x", 1);
    EXPECT_EQ(nw, static_cast<ssize_t>(resource::ERR_NOTCONN));

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

// Handle flags / fcntl tests

TEST(socket_test, handle_flags_default_zero) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    uint32_t flags = 0xFFFF;
    ASSERT_EQ(resource::get_handle_flags(task->handles, h, &flags), resource::HANDLE_OK);
    EXPECT_EQ(flags, 0u);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

TEST(socket_test, status_flags_set_and_get) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::set_status_flags(obj, fs::O_NONBLOCK | fs::O_CLOEXEC);
    EXPECT_EQ(resource::get_status_flags(obj), static_cast<uint32_t>(fs::O_NONBLOCK));

    resource::set_status_flags(obj, 0);
    EXPECT_EQ(resource::get_status_flags(obj), 0u);

    resource::resource_release(obj);
}

TEST(socket_test, handle_flags_invalid_handle) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    uint32_t flags = 0;
    EXPECT_EQ(resource::get_handle_flags(task->handles, -1, &flags), resource::HANDLE_ERR_NOENT);
    EXPECT_EQ(resource::set_handle_flags(task->handles, -1, 0), resource::HANDLE_ERR_NOENT);
    EXPECT_EQ(resource::get_handle_flags(task->handles, 9999, &flags), resource::HANDLE_ERR_NOENT);
}

// Non-blocking socket read/write via status flags

TEST(socket_test, nonblock_socketpair_read_eagain) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::set_status_flags(obj_a, fs::O_NONBLOCK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    char buf[8] = {};
    ssize_t nr = resource::read(task, h0, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(resource::ERR_AGAIN));

    EXPECT_EQ(resource::close(task, h0), resource::OK);
    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

TEST(socket_test, nonblock_socketpair_read_with_data) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::set_status_flags(obj_b, fs::O_NONBLOCK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    ASSERT_EQ(resource::write(task, h0, "data", 4), static_cast<ssize_t>(4));

    char buf[32] = {};
    ssize_t nr = resource::read(task, h1, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(buf, "data", 4), 0);

    EXPECT_EQ(resource::close(task, h0), resource::OK);
    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

TEST(socket_test, nonblock_socketpair_eof_not_eagain) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::set_status_flags(obj_b, fs::O_NONBLOCK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    EXPECT_EQ(resource::close(task, h0), resource::OK);

    char buf[8] = {};
    ssize_t nr = resource::read(task, h1, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF, not EAGAIN

    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

// Listener state tests

TEST(socket_test, listener_state_create_destroy) {
    auto ls = rc::make_kref<socket::listener_state>();
    ASSERT_TRUE(static_cast<bool>(ls));

    ls->lock = sync::SPINLOCK_INIT;
    ls->closed = false;
    ls->accept_queue.init();
    ls->accept_wq.init();
    ls->backlog = 16;
    ls->pending_count = 0;

    EXPECT_FALSE(ls->closed);
    EXPECT_TRUE(ls->accept_queue.empty());
    EXPECT_EQ(ls->pending_count, 0u);
}

// Channel ref counting

TEST(socket_test, channel_refcount_after_socketpair) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    auto* sock_a = static_cast<socket::unix_socket*>(obj_a->impl);
    auto* sock_b = static_cast<socket::unix_socket*>(obj_b->impl);
    ASSERT_NOT_NULL(sock_a);
    ASSERT_NOT_NULL(sock_b);

    EXPECT_EQ(sock_a->channel.ptr(), sock_b->channel.ptr());
    EXPECT_EQ(sock_a->channel->ref_count(), 2u);

    resource::resource_release(obj_a);
    EXPECT_EQ(sock_b->channel->ref_count(), 1u);

    resource::resource_release(obj_b);
}

// Socket type validation

TEST(socket_test, socket_handle_has_socket_type) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    resource::resource_object* held = nullptr;
    ASSERT_EQ(resource::get_handle_object(task->handles, h, 0, &held), resource::HANDLE_OK);
    EXPECT_EQ(held->type, resource::resource_type::SOCKET);
    resource::resource_release(held);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

TEST(socket_test, close_invalid_handle_returns_badf) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    EXPECT_EQ(resource::close(task, -1), resource::ERR_BADF);
    EXPECT_EQ(resource::close(task, 9999), resource::ERR_BADF);
}

// VFS socket node

TEST(socket_test, node_type_socket_in_fstypes) {
    fs::vattr attr;
    attr.type = fs::node_type::socket;
    attr.size = 0;
    EXPECT_EQ(attr.type, fs::node_type::socket);
}

// Double close safety

TEST(socket_test, double_close_returns_badf) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    EXPECT_EQ(resource::close(task, h0), resource::OK);
    EXPECT_EQ(resource::close(task, h0), resource::ERR_BADF);

    EXPECT_EQ(resource::close(task, h1), resource::OK);
    EXPECT_EQ(resource::close(task, h1), resource::ERR_BADF);
}

// get_handle_object with flags output

TEST(socket_test, get_handle_object_returns_flags) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    ASSERT_EQ(resource::set_handle_flags(task->handles, h, resource::RESOURCE_HANDLE_CLOEXEC), resource::HANDLE_OK);

    resource::resource_object* out = nullptr;
    uint32_t out_flags = 0;
    ASSERT_EQ(resource::get_handle_object(task->handles, h, resource::RIGHT_READ, &out, &out_flags),
        resource::HANDLE_OK);
    EXPECT_EQ(out_flags, resource::RESOURCE_HANDLE_CLOEXEC);
    resource::resource_release(out);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

// Blocking stream sends

struct blocking_send_run {
    resource::resource_object* obj;
    ssize_t result;
    sync::atomic<uint32_t> done;
};

static blocking_send_run g_blocking_send;
static uint8_t g_blocking_send_bytes[4096];

static void run_blocking_send(void*) {
    blocking_send_run& run = g_blocking_send;
    run.result = run.obj->ops->socket->sendto(run.obj, g_blocking_send_bytes, sizeof(g_blocking_send_bytes),
                                              0, nullptr, 0);
    run.done.store_release(1);
    sched::exit(0);
}

TEST(socket_test, a_blocking_stream_send_waits_for_room_and_sends_everything) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    const resource::socket_ops* ops = obj_a->ops->socket;

    // The stream starts full, so the send finds no room at all
    size_t queued = 0;
    ssize_t n = 0;
    while ((n = ops->sendto(obj_a, g_blocking_send_bytes, sizeof(g_blocking_send_bytes), net::inet::MSG_DONTWAIT,
                            nullptr, 0)) > 0) {
        queued += static_cast<size_t>(n);
    }
    ASSERT_EQ(n, static_cast<ssize_t>(resource::ERR_AGAIN));

    g_blocking_send.obj = obj_a;
    g_blocking_send.result = 0;
    g_blocking_send.done.store_relaxed(0);
    sched::task* sender = nullptr;
    RUN_ELEVATED({
        sender = sched::create_kernel_task(run_blocking_send, nullptr, "blocking_send", sched::TASK_FLAG_ELEVATED);
        if (sender) {
            sched::enqueue(sender);
        }
    });
    ASSERT_NOT_NULL(sender);

    // Draining in pieces smaller than the send catches a sender that stops at its first room
    size_t expected = queued + sizeof(g_blocking_send_bytes);
    size_t drained = 0;
    uint8_t piece[512];
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (drained < expected && clock::now_ns() < deadline) {
        ssize_t got = obj_b->ops->read(obj_b, piece, sizeof(piece), fs::O_NONBLOCK);
        if (got > 0) {
            drained += static_cast<size_t>(got);
        }
    }

    EXPECT_EQ(drained, expected);
    EXPECT_TRUE(test_helpers::spin_wait(g_blocking_send.done));
    EXPECT_EQ(g_blocking_send.result, static_cast<ssize_t>(sizeof(g_blocking_send_bytes)));

    // Closing the reader first lets a sender still waiting give up before its socket goes away
    resource::resource_release(obj_b);
    (void)test_helpers::spin_wait(g_blocking_send.done);
    resource::resource_release(obj_a);
}

// A kernel task cannot receive the signal, so only the results are checked here
TEST(socket_test, stream_writes_to_a_closed_peer_break_with_or_without_the_signal) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_release(obj_b);

    const resource::socket_ops* ops = obj_a->ops->socket;
    EXPECT_EQ(ops->sendto(obj_a, "x", 1, net::inet::MSG_NOSIGNAL, nullptr, 0), resource::ERR_PIPE);
    EXPECT_EQ(ops->sendto(obj_a, "x", 1, 0, nullptr, 0), resource::ERR_PIPE);
    EXPECT_EQ(obj_a->ops->write(obj_a, "x", 1, 0), resource::ERR_PIPE);

    resource::resource_release(obj_a);
}
