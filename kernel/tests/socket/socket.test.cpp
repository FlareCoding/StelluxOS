#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "socket/unix_socket.h"
#include "socket/ring_buffer.h"
#include "socket/listener.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"
#include "fs/fstypes.h"
#include "fs/socket_node.h"

TEST_SUITE(socket_test);

// ---------------------------------------------------------------------------
// Ring buffer tests
// ---------------------------------------------------------------------------

TEST(socket_test, ring_buffer_create_destroy) {
    auto* rb = socket::ring_buffer_create(socket::DEFAULT_CAPACITY);
    ASSERT_NOT_NULL(rb);
    ASSERT_NOT_NULL(rb->data);
    EXPECT_GT(rb->capacity, socket::DEFAULT_CAPACITY);
    EXPECT_EQ(rb->head, 0u);
    EXPECT_EQ(rb->tail, 0u);
    EXPECT_FALSE(rb->writer_closed);
    EXPECT_FALSE(rb->reader_closed);
    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_write_read_basic) {
    auto* rb = socket::ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);

    const uint8_t msg[] = "hello ring";
    ssize_t nw = socket::ring_buffer_write(rb, msg, 10);
    EXPECT_EQ(nw, static_cast<ssize_t>(10));

    uint8_t buf[32] = {};
    ssize_t nr = socket::ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(10));
    EXPECT_EQ(string::memcmp(buf, msg, 10), 0);

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_multiple_writes_single_read) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(socket::ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("aaa"), 3),
        static_cast<ssize_t>(3));
    ASSERT_EQ(socket::ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("bbb"), 3),
        static_cast<ssize_t>(3));
    ASSERT_EQ(socket::ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("ccc"), 3),
        static_cast<ssize_t>(3));

    uint8_t buf[32] = {};
    ssize_t nr = socket::ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(9));
    EXPECT_EQ(string::memcmp(buf, "aaabbbccc", 9), 0);

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_short_read) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(socket::ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("xyz"), 3),
        static_cast<ssize_t>(3));

    uint8_t buf[1] = {};
    ssize_t nr = socket::ring_buffer_read(rb, buf, 1);
    EXPECT_EQ(nr, static_cast<ssize_t>(1));
    EXPECT_EQ(buf[0], static_cast<uint8_t>('x'));

    nr = socket::ring_buffer_read(rb, buf, 1);
    EXPECT_EQ(nr, static_cast<ssize_t>(1));
    EXPECT_EQ(buf[0], static_cast<uint8_t>('y'));

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_eof_after_close_write) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(socket::ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("ab"), 2),
        static_cast<ssize_t>(2));
    socket::ring_buffer_close_write(rb);

    uint8_t buf[32] = {};
    ssize_t nr = socket::ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(2));

    nr = socket::ring_buffer_read(rb, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_epipe_after_close_read) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    socket::ring_buffer_close_read(rb);

    ssize_t nw = socket::ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("x"), 1);
    EXPECT_EQ(nw, static_cast<ssize_t>(resource::ERR_PIPE));

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_empty_returns_eagain) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    uint8_t buf[8] = {};
    ssize_t nr = socket::ring_buffer_read(rb, buf, sizeof(buf), true);
    EXPECT_EQ(nr, static_cast<ssize_t>(resource::ERR_AGAIN));

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_full_returns_eagain) {
    auto* rb = socket::ring_buffer_create(16);
    ASSERT_NOT_NULL(rb);

    uint8_t fill[64];
    string::memset(fill, 'A', sizeof(fill));

    // Fill the buffer
    ssize_t nw = socket::ring_buffer_write(rb, fill, sizeof(fill));
    EXPECT_GT(nw, static_cast<ssize_t>(0));

    // Now try non-blocking write when full
    nw = socket::ring_buffer_write(rb, fill, 1, true);
    EXPECT_EQ(nw, static_cast<ssize_t>(resource::ERR_AGAIN));

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_with_data_returns_data) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    ASSERT_EQ(socket::ring_buffer_write(rb, reinterpret_cast<const uint8_t*>("test"), 4),
        static_cast<ssize_t>(4));

    uint8_t buf[32] = {};
    ssize_t nr = socket::ring_buffer_read(rb, buf, sizeof(buf), true);
    EXPECT_EQ(nr, static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(buf, "test", 4), 0);

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_nonblock_eof_returns_zero) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    socket::ring_buffer_close_write(rb);

    uint8_t buf[8] = {};
    ssize_t nr = socket::ring_buffer_read(rb, buf, sizeof(buf), true);
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF, not EAGAIN

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_zero_length_returns_inval) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    uint8_t buf[1] = {};
    EXPECT_EQ(socket::ring_buffer_read(rb, buf, 0), static_cast<ssize_t>(resource::ERR_INVAL));
    EXPECT_EQ(socket::ring_buffer_write(rb, buf, 0), static_cast<ssize_t>(resource::ERR_INVAL));

    socket::ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_null_args_returns_inval) {
    auto* rb = socket::ring_buffer_create(256);
    ASSERT_NOT_NULL(rb);

    EXPECT_EQ(socket::ring_buffer_read(nullptr, nullptr, 1), static_cast<ssize_t>(resource::ERR_INVAL));
    EXPECT_EQ(socket::ring_buffer_read(rb, nullptr, 1), static_cast<ssize_t>(resource::ERR_INVAL));
    EXPECT_EQ(socket::ring_buffer_write(nullptr, nullptr, 1), static_cast<ssize_t>(resource::ERR_INVAL));
    EXPECT_EQ(socket::ring_buffer_write(rb, nullptr, 1), static_cast<ssize_t>(resource::ERR_INVAL));

    socket::ring_buffer_destroy(rb);
}

// ---------------------------------------------------------------------------
// Socket pair creation and data flow
// ---------------------------------------------------------------------------

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
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
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

// ---------------------------------------------------------------------------
// Unbound socket tests
// ---------------------------------------------------------------------------

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
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj, resource::resource_type::SOCKET,
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
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    ssize_t nw = resource::write(task, h, "x", 1);
    EXPECT_EQ(nw, static_cast<ssize_t>(resource::ERR_NOTCONN));

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

// ---------------------------------------------------------------------------
// Handle flags / fcntl tests
// ---------------------------------------------------------------------------

TEST(socket_test, handle_flags_default_zero) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    uint32_t flags = 0xFFFF;
    ASSERT_EQ(resource::get_handle_flags(&task->handles, h, &flags), resource::HANDLE_OK);
    EXPECT_EQ(flags, 0u);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

TEST(socket_test, handle_flags_set_and_get) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    ASSERT_EQ(resource::set_handle_flags(&task->handles, h, fs::O_NONBLOCK), resource::HANDLE_OK);

    uint32_t flags = 0;
    ASSERT_EQ(resource::get_handle_flags(&task->handles, h, &flags), resource::HANDLE_OK);
    EXPECT_BITS_SET(flags, fs::O_NONBLOCK);

    ASSERT_EQ(resource::set_handle_flags(&task->handles, h, 0), resource::HANDLE_OK);
    ASSERT_EQ(resource::get_handle_flags(&task->handles, h, &flags), resource::HANDLE_OK);
    EXPECT_EQ(flags, 0u);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

TEST(socket_test, handle_flags_invalid_handle) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    uint32_t flags = 0;
    EXPECT_EQ(resource::get_handle_flags(&task->handles, -1, &flags), resource::HANDLE_ERR_NOENT);
    EXPECT_EQ(resource::set_handle_flags(&task->handles, -1, 0), resource::HANDLE_ERR_NOENT);
    EXPECT_EQ(resource::get_handle_flags(&task->handles, 9999, &flags), resource::HANDLE_ERR_NOENT);
}

TEST(socket_test, handle_flags_cleared_on_close) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    ASSERT_EQ(resource::set_handle_flags(&task->handles, h, fs::O_NONBLOCK), resource::HANDLE_OK);
    EXPECT_EQ(resource::close(task, h), resource::OK);

    uint32_t flags = 0;
    EXPECT_EQ(resource::get_handle_flags(&task->handles, h, &flags), resource::HANDLE_ERR_NOENT);
}

// ---------------------------------------------------------------------------
// Non-blocking socket read/write via handle flags
// ---------------------------------------------------------------------------

TEST(socket_test, nonblock_socketpair_read_eagain) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    ASSERT_EQ(resource::set_handle_flags(&task->handles, h0, fs::O_NONBLOCK), resource::HANDLE_OK);

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

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    ASSERT_EQ(resource::set_handle_flags(&task->handles, h1, fs::O_NONBLOCK), resource::HANDLE_OK);

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

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    ASSERT_EQ(resource::set_handle_flags(&task->handles, h1, fs::O_NONBLOCK), resource::HANDLE_OK);
    EXPECT_EQ(resource::close(task, h0), resource::OK);

    char buf[8] = {};
    ssize_t nr = resource::read(task, h1, buf, sizeof(buf));
    EXPECT_EQ(nr, static_cast<ssize_t>(0)); // EOF, not EAGAIN

    EXPECT_EQ(resource::close(task, h1), resource::OK);
}

// ---------------------------------------------------------------------------
// Listener state tests
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Channel ref counting
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Socket type validation
// ---------------------------------------------------------------------------

TEST(socket_test, socket_handle_has_socket_type) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    const resource::handle_entry& entry = task->handles.entries[static_cast<uint32_t>(h)];
    EXPECT_TRUE(entry.used);
    EXPECT_EQ(entry.type, resource::resource_type::SOCKET);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

TEST(socket_test, close_invalid_handle_returns_badf) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    EXPECT_EQ(resource::close(task, -1), resource::ERR_BADF);
    EXPECT_EQ(resource::close(task, 9999), resource::ERR_BADF);
}

// ---------------------------------------------------------------------------
// VFS socket node
// ---------------------------------------------------------------------------

TEST(socket_test, node_type_socket_in_fstypes) {
    fs::vattr attr;
    attr.type = fs::node_type::socket;
    attr.size = 0;
    EXPECT_EQ(attr.type, fs::node_type::socket);
}

// ---------------------------------------------------------------------------
// Double close safety
// ---------------------------------------------------------------------------

TEST(socket_test, double_close_returns_badf) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_t h0 = -1, h1 = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_a, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h0), resource::HANDLE_OK);
    resource::resource_release(obj_a);
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj_b, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h1), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    EXPECT_EQ(resource::close(task, h0), resource::OK);
    EXPECT_EQ(resource::close(task, h0), resource::ERR_BADF);

    EXPECT_EQ(resource::close(task, h1), resource::OK);
    EXPECT_EQ(resource::close(task, h1), resource::ERR_BADF);
}

// ---------------------------------------------------------------------------
// get_handle_object with flags output
// ---------------------------------------------------------------------------

TEST(socket_test, get_handle_object_returns_flags) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&obj), resource::OK);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::alloc_handle(&task->handles, obj, resource::resource_type::SOCKET,
        resource::RIGHT_READ | resource::RIGHT_WRITE, &h), resource::HANDLE_OK);
    resource::resource_release(obj);

    ASSERT_EQ(resource::set_handle_flags(&task->handles, h, fs::O_NONBLOCK), resource::HANDLE_OK);

    resource::resource_object* out = nullptr;
    uint32_t out_flags = 0;
    ASSERT_EQ(resource::get_handle_object(&task->handles, h, resource::RIGHT_READ, &out, &out_flags),
        resource::HANDLE_OK);
    EXPECT_BITS_SET(out_flags, fs::O_NONBLOCK);
    resource::resource_release(out);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}
