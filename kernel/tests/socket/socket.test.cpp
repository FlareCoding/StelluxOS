#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "socket/unix_socket.h"
#include "resource/socket_ops.h"
#include "net/inet.h"
#include "dynpriv/dynpriv.h"
#include "common/ring_buffer.h"
#include "sync/poll.h"
#include "socket/listener.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"
#include "fs/fstypes.h"
#include "fs/socket_node.h"
#include "mm/heap.h"
#include "syscall/handlers/sys_socket.h"
#include "hw/cpu.h"
#include "fs/fs.h"

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

TEST(socket_test, ring_buffer_polls_shut_once_either_side_closes) {
    auto* reader_gone = ring_buffer_create(64);
    auto* writer_gone = ring_buffer_create(64);
    ASSERT_NOT_NULL(reader_gone);
    ASSERT_NOT_NULL(writer_gone);

    ring_buffer_close_read(reader_gone);
    ring_buffer_close_write(writer_gone);
    EXPECT_EQ(ring_buffer_poll_read(reader_gone, nullptr), sync::POLL_HUP);
    EXPECT_EQ(ring_buffer_poll_write(writer_gone, nullptr), sync::POLL_OUT | sync::POLL_ERR);

    ring_buffer_destroy(reader_gone);
    ring_buffer_destroy(writer_gone);
}

TEST(socket_test, ring_buffer_marks_exactly_the_bytes_one_write_takes) {
    auto* rb = ring_buffer_create(8);
    ASSERT_NOT_NULL(rb);

    // Ten queued bytes leave room for five, so the marked write takes five
    uint8_t bytes[16] = {};
    ring_buffer_mark mark = {};
    ASSERT_EQ(ring_buffer_write(rb, bytes, 10), static_cast<ssize_t>(10));
    EXPECT_EQ(ring_buffer_write_marked(rb, bytes, sizeof(bytes), &mark, true), static_cast<ssize_t>(5));
    EXPECT_EQ(mark.position, static_cast<size_t>(10));
    EXPECT_EQ(mark.length, static_cast<size_t>(5));

    ring_buffer_mark_list marks;
    marks.init();
    ring_buffer_take_marks(rb, marks);
    EXPECT_EQ(marks.pop_front(), &mark);

    ring_buffer_destroy(rb);
}

static void record_reached_mark(ring_buffer_mark* mark, void* seen) {
    *static_cast<ring_buffer_mark**>(seen) = mark;
}

TEST(socket_test, ring_buffer_marked_reads_stop_where_the_marked_bytes_end) {
    auto* rb = ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>("abcdefg");
    ring_buffer_mark mark = {};
    ASSERT_EQ(ring_buffer_write(rb, bytes, 3), static_cast<ssize_t>(3));
    ASSERT_EQ(ring_buffer_write_marked(rb, bytes + 3, 2, &mark, true), static_cast<ssize_t>(2));
    ASSERT_EQ(ring_buffer_write(rb, bytes + 5, 2), static_cast<ssize_t>(2));

    // A peek stops at the same place and shows the mark while leaving it queued
    uint8_t buf[16] = {};
    ring_buffer_mark* seen = nullptr;
    EXPECT_EQ(ring_buffer_peek_marked(rb, buf, sizeof(buf), record_reached_mark, &seen), static_cast<ssize_t>(5));
    EXPECT_EQ(seen, &mark);

    ring_buffer_mark* taken = nullptr;
    EXPECT_EQ(ring_buffer_read_marked(rb, buf, sizeof(buf), &taken), static_cast<ssize_t>(5));
    EXPECT_EQ(string::memcmp(buf, "abcde", 5), 0);
    EXPECT_EQ(taken, &mark);

    EXPECT_EQ(ring_buffer_read_marked(rb, buf, sizeof(buf), &taken), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(buf, "fg", 2), 0);
    EXPECT_NULL(taken);

    EXPECT_EQ(ring_buffer_read_marked(rb, buf, sizeof(buf), &taken), static_cast<ssize_t>(RB_ERR_AGAIN));

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_leaves_the_mark_alone_when_a_write_writes_nothing) {
    auto* rb = ring_buffer_create(8);
    ASSERT_NOT_NULL(rb);

    uint8_t fill[15] = {};
    ring_buffer_mark mark = {};
    ASSERT_EQ(ring_buffer_write(rb, fill, sizeof(fill)), static_cast<ssize_t>(sizeof(fill)));
    EXPECT_EQ(ring_buffer_write_marked(rb, fill, 1, &mark, true), static_cast<ssize_t>(RB_ERR_AGAIN));
    EXPECT_FALSE(mark.link.is_linked());

    ring_buffer_close_read(rb);
    EXPECT_EQ(ring_buffer_write_marked(rb, fill, 1, &mark, false), static_cast<ssize_t>(RB_ERR_PIPE));
    EXPECT_FALSE(mark.link.is_linked());

    ring_buffer_destroy(rb);
}

TEST(socket_test, ring_buffer_hands_every_mark_to_its_owner) {
    auto* rb = ring_buffer_create(64);
    ASSERT_NOT_NULL(rb);

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>("ab");
    ring_buffer_mark first = {};
    ring_buffer_mark second = {};
    ASSERT_EQ(ring_buffer_write_marked(rb, bytes, 1, &first, true), static_cast<ssize_t>(1));
    ASSERT_EQ(ring_buffer_write_marked(rb, bytes + 1, 1, &second, true), static_cast<ssize_t>(1));

    ring_buffer_mark_list marks;
    marks.init();
    ring_buffer_take_marks(rb, marks);
    EXPECT_EQ(marks.pop_front(), &first);
    EXPECT_EQ(marks.pop_front(), &second);
    EXPECT_NULL(marks.pop_front());

    // Taken marks no longer stop a read
    uint8_t buf[4] = {};
    ring_buffer_mark* taken = nullptr;
    EXPECT_EQ(ring_buffer_read_marked(rb, buf, sizeof(buf), &taken), static_cast<ssize_t>(2));
    EXPECT_NULL(taken);

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
    size_t len;
    ssize_t result;
    sync::atomic<uint32_t> done;
};

static blocking_send_run g_blocking_sends[2];
static uint8_t g_blocking_send_bytes[4096];

static void run_blocking_send(void* arg) {
    blocking_send_run& run = *static_cast<blocking_send_run*>(arg);
    run.result = run.obj->ops->socket->sendto(run.obj, g_blocking_send_bytes, run.len, 0, nullptr, 0);
    run.done.store_release(1);
    sched::exit(0);
}

static void prepare_blocking_send(blocking_send_run& run, resource::resource_object* obj, size_t len) {
    run.obj = obj;
    run.len = len;
    run.result = 0;
    run.done.store_relaxed(0);
}

// Fills the stream `obj` sends into, so the next blocking send finds no room at all
static size_t fill_stream(resource::resource_object* obj) {
    size_t queued = 0;
    ssize_t n = 0;
    while ((n = obj->ops->socket->sendto(obj, g_blocking_send_bytes, sizeof(g_blocking_send_bytes),
                                         net::inet::MSG_DONTWAIT, nullptr, 0)) > 0) {
        queued += static_cast<size_t>(n);
    }

    return n == resource::ERR_AGAIN ? queued : 0;
}

TEST(socket_test, a_blocking_stream_send_waits_for_room_and_sends_everything) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    size_t queued = fill_stream(obj_a);
    ASSERT_TRUE(queued > 0);

    blocking_send_run& send = g_blocking_sends[0];
    prepare_blocking_send(send, obj_a, sizeof(g_blocking_send_bytes));
    sched::task* sender = nullptr;
    RUN_ELEVATED({
        sender = sched::create_kernel_task(run_blocking_send, &send, "blocking_send", sched::TASK_FLAG_ELEVATED);
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
    EXPECT_TRUE(test_helpers::spin_wait(send.done));
    EXPECT_EQ(send.result, static_cast<ssize_t>(sizeof(g_blocking_send_bytes)));

    // Closing the reader first lets a sender still waiting give up before its socket goes away
    resource::resource_release(obj_b);
    (void)test_helpers::spin_wait(send.done);
    resource::resource_release(obj_a);
}

TEST(socket_test, one_read_wakes_every_send_waiting_for_the_room_it_frees) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    ASSERT_TRUE(fill_stream(obj_a) > 0);

    // Both senders fall asleep on the full stream, then one read frees room for the two of them
    constexpr size_t SEND_LEN = 100;
    sched::task* senders[2] = {};
    for (uint32_t i = 0; i < 2; i++) {
        prepare_blocking_send(g_blocking_sends[i], obj_a, SEND_LEN);
        senders[i] = test_helpers::start_pinned_task(run_blocking_send, &g_blocking_sends[i], "blocking_send");
        ASSERT_NOT_NULL(senders[i]);
        EXPECT_TRUE(test_helpers::blocks_before_deadline(senders[i]));
    }

    uint8_t room[2 * SEND_LEN];
    EXPECT_EQ(obj_b->ops->read(obj_b, room, sizeof(room), fs::O_NONBLOCK), static_cast<ssize_t>(sizeof(room)));
    EXPECT_TRUE(test_helpers::spin_wait(g_blocking_sends[0].done));
    EXPECT_TRUE(test_helpers::spin_wait(g_blocking_sends[1].done));
    EXPECT_EQ(g_blocking_sends[0].result, static_cast<ssize_t>(SEND_LEN));
    EXPECT_EQ(g_blocking_sends[1].result, static_cast<ssize_t>(SEND_LEN));

    // Closing the reader first lets a sender still waiting give up before its socket goes away
    resource::resource_release(obj_b);
    for (sched::task* t : senders) {
        test_helpers::unpin(t);
    }

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

// Stream polling

constexpr uint32_t STREAM_DOWN = sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_OUT | sync::POLL_HUP;

TEST(socket_test, a_unix_stream_hangs_up_without_an_error_when_its_peer_closes) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    EXPECT_EQ(obj_b->ops->poll(obj_b, nullptr), sync::POLL_OUT);

    resource::resource_release(obj_a);
    EXPECT_EQ(obj_b->ops->poll(obj_b, nullptr), STREAM_DOWN);

    resource::resource_release(obj_b);
}

TEST(socket_test, a_unix_stream_polls_half_closed_until_both_directions_shut) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    const resource::socket_ops* ops = obj_a->ops->socket;

    // One shut direction ends that stream at the peer without hanging up
    EXPECT_EQ(ops->shutdown(obj_a, resource::SHUT_WR), resource::OK);
    EXPECT_EQ(obj_a->ops->poll(obj_a, nullptr), sync::POLL_OUT);
    EXPECT_EQ(obj_b->ops->poll(obj_b, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_OUT);

    EXPECT_EQ(ops->shutdown(obj_a, resource::SHUT_RD), resource::OK);
    EXPECT_EQ(obj_a->ops->poll(obj_a, nullptr), STREAM_DOWN);
    EXPECT_EQ(obj_b->ops->poll(obj_b, nullptr), STREAM_DOWN);

    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, a_full_unix_stream_polls_writable_once_its_peer_stops_reading) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    const resource::socket_ops* ops = obj_a->ops->socket;

    const uint8_t chunk[256] = {};
    ssize_t n = 1;
    while (n > 0) {
        n = ops->sendto(obj_b, chunk, sizeof(chunk), net::inet::MSG_DONTWAIT, nullptr, 0);
    }
    EXPECT_EQ(n, static_cast<ssize_t>(resource::ERR_AGAIN));
    EXPECT_EQ(obj_b->ops->poll(obj_b, nullptr), 0u);

    // Sends fail at once after the peer stops reading, so a poller waiting for room must not keep waiting
    EXPECT_EQ(ops->shutdown(obj_a, resource::SHUT_RD), resource::OK);
    EXPECT_EQ(obj_b->ops->poll(obj_b, nullptr), sync::POLL_OUT);
    EXPECT_EQ(obj_a->ops->poll(obj_a, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_OUT);

    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

// Handle batches

static sync::atomic<uint32_t> g_passenger_closes;
static sync::atomic<uint32_t> g_passenger_closes_under_spinlock;

// A spinlock holder runs with interrupts off, where a closing object must never be torn down
static void close_passenger(resource::resource_object*) {
    g_passenger_closes.fetch_add_relaxed(1);
    if (!cpu::irqs_enabled()) {
        g_passenger_closes_under_spinlock.fetch_add_relaxed(1);
    }
}

static const resource::resource_ops g_passenger_ops = {
    .close = close_passenger,
};

// An object to pass around, whose teardown counts in g_passenger_closes
static resource::resource_object* make_passenger() {
    auto* obj = heap::kalloc_new<resource::resource_object>();
    if (obj) {
        obj->type = resource::resource_type::FILE;
        obj->ops = &g_passenger_ops;
    }

    return obj;
}

// A batch holding one more reference to `obj`
static resource::handle_batch* batch_of(resource::resource_object* obj) {
    resource::handle_batch* batch = resource::create_handle_batch(1);
    if (batch) {
        resource::resource_add_ref(obj);
        batch->entries[0] = {obj, obj->type, resource::RIGHT_READ};
    }

    return batch;
}

static ssize_t send_with(resource::resource_object* obj, const char* bytes, size_t len, resource::handle_batch* batch,
                         uint32_t flags = net::inet::MSG_DONTWAIT) {
    return obj->ops->socket->sendmsg(obj, bytes, len, flags, nullptr, 0, batch);
}

static ssize_t receive_with(resource::resource_object* obj, char* buf, size_t len, resource::handle_batch** batch,
                            uint32_t flags = net::inet::MSG_DONTWAIT) {
    return obj->ops->socket->recvmsg(obj, buf, len, flags, nullptr, nullptr, batch);
}

TEST(socket_test, a_read_runs_into_the_bytes_handles_came_with_and_takes_them) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    EXPECT_EQ(send_with(obj_a, "hello", 5, nullptr), static_cast<ssize_t>(5));
    EXPECT_EQ(send_with(obj_a, "world", 5, batch_of(passenger)), static_cast<ssize_t>(5));

    char buf[16] = {};
    resource::handle_batch* batch = nullptr;
    EXPECT_EQ(receive_with(obj_b, buf, sizeof(buf), &batch), static_cast<ssize_t>(10));
    EXPECT_EQ(string::memcmp(buf, "helloworld", 10), 0);

    ASSERT_NOT_NULL(batch);
    EXPECT_EQ(batch->count, 1u);
    EXPECT_EQ(batch->entries[0].obj, passenger);
    EXPECT_EQ(batch->entries[0].rights, resource::RIGHT_READ);

    resource::handle_batch_release(batch);
    resource::resource_release(passenger);
    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, a_read_takes_at_most_one_batch) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::resource_object* first = make_passenger();
    resource::resource_object* second = make_passenger();
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    EXPECT_EQ(send_with(obj_a, "ab", 2, batch_of(first)), static_cast<ssize_t>(2));
    EXPECT_EQ(send_with(obj_a, "cd", 2, batch_of(second)), static_cast<ssize_t>(2));

    char buf[16] = {};
    resource::handle_batch* batch = nullptr;
    EXPECT_EQ(receive_with(obj_b, buf, sizeof(buf), &batch), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(buf, "ab", 2), 0);
    EXPECT_TRUE(batch && batch->entries[0].obj == first);
    resource::handle_batch_release(batch);

    EXPECT_EQ(receive_with(obj_b, buf, sizeof(buf), &batch), static_cast<ssize_t>(2));
    EXPECT_EQ(string::memcmp(buf, "cd", 2), 0);
    EXPECT_TRUE(batch && batch->entries[0].obj == second);
    resource::handle_batch_release(batch);

    resource::resource_release(first);
    resource::resource_release(second);
    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, only_the_read_that_consumes_the_first_handle_byte_takes_the_batch) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    EXPECT_EQ(send_with(obj_a, "hello", 5, nullptr), static_cast<ssize_t>(5));
    EXPECT_EQ(send_with(obj_a, "world", 5, batch_of(passenger)), static_cast<ssize_t>(5));

    // Short of the handle bytes nothing is taken, and the read reaching them takes the batch
    char buf[16] = {};
    resource::handle_batch* batch = nullptr;
    EXPECT_EQ(receive_with(obj_b, buf, 3, &batch), static_cast<ssize_t>(3));
    EXPECT_NULL(batch);

    EXPECT_EQ(receive_with(obj_b, buf, 4, &batch), static_cast<ssize_t>(4));
    EXPECT_EQ(string::memcmp(buf, "lowo", 4), 0);
    EXPECT_TRUE(batch && batch->entries[0].obj == passenger);
    resource::handle_batch_release(batch);

    EXPECT_EQ(receive_with(obj_b, buf, sizeof(buf), &batch), static_cast<ssize_t>(3));
    EXPECT_EQ(string::memcmp(buf, "rld", 3), 0);
    EXPECT_NULL(batch);

    resource::resource_release(passenger);
    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, a_peek_stops_where_the_handle_bytes_end_and_shares_their_batch) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    EXPECT_EQ(send_with(obj_a, "ab", 2, batch_of(passenger)), static_cast<ssize_t>(2));
    EXPECT_EQ(send_with(obj_a, "cd", 2, nullptr), static_cast<ssize_t>(2));

    char buf[16] = {};
    resource::handle_batch* peeked = nullptr;
    uint32_t peek = net::inet::MSG_PEEK | net::inet::MSG_DONTWAIT;
    EXPECT_EQ(receive_with(obj_b, buf, sizeof(buf), &peeked, peek), static_cast<ssize_t>(2));
    ASSERT_NOT_NULL(peeked);

    resource::handle_batch* taken = nullptr;
    EXPECT_EQ(receive_with(obj_b, buf, sizeof(buf), &taken), static_cast<ssize_t>(2));
    EXPECT_EQ(taken, peeked);

    // The object lives until the last holder of the shared batch lets go
    uint32_t closes = g_passenger_closes.load_relaxed();
    resource::resource_release(passenger);
    resource::handle_batch_release(peeked);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes);

    resource::handle_batch_release(taken);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);

    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, plain_reads_and_receives_drop_the_handles_they_pass) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::resource_object* read_past = make_passenger();
    resource::resource_object* received_past = make_passenger();
    ASSERT_NOT_NULL(read_past);
    ASSERT_NOT_NULL(received_past);

    EXPECT_EQ(send_with(obj_a, "x", 1, batch_of(read_past)), static_cast<ssize_t>(1));
    EXPECT_EQ(send_with(obj_a, "y", 1, batch_of(received_past)), static_cast<ssize_t>(1));
    resource::resource_release(read_past);
    resource::resource_release(received_past);

    // The queued batches hold the last references, so dropping one tears its object down
    uint32_t closes = g_passenger_closes.load_relaxed();
    char c = 0;
    EXPECT_EQ(obj_b->ops->read(obj_b, &c, 1, fs::O_NONBLOCK), static_cast<ssize_t>(1));
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);

    EXPECT_EQ(obj_b->ops->socket->recvfrom(obj_b, &c, 1, net::inet::MSG_DONTWAIT, nullptr, nullptr),
              static_cast<ssize_t>(1));
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 2);

    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, a_send_that_sends_nothing_leaves_the_batch_with_the_caller) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    ASSERT_TRUE(fill_stream(obj_a) > 0);
    resource::handle_batch* batch = batch_of(passenger);
    EXPECT_EQ(send_with(obj_a, "x", 1, batch), static_cast<ssize_t>(resource::ERR_AGAIN));
    EXPECT_EQ(send_with(obj_a, "x", 0, batch), static_cast<ssize_t>(0));

    resource::resource_release(obj_b);
    EXPECT_EQ(send_with(obj_a, "x", 1, batch, net::inet::MSG_NOSIGNAL), static_cast<ssize_t>(resource::ERR_PIPE));

    // Still the caller's, so releasing it here is the only release
    uint32_t closes = g_passenger_closes.load_relaxed();
    resource::handle_batch_release(batch);
    resource::resource_release(passenger);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);

    resource::resource_release(obj_a);
}

TEST(socket_test, batches_with_unix_sockets_or_empty_entries_are_refused) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    resource::handle_batch* carrying_socket = batch_of(obj_b);
    ASSERT_NOT_NULL(carrying_socket);
    EXPECT_EQ(send_with(obj_a, "x", 1, carrying_socket), static_cast<ssize_t>(resource::ERR_UNSUP));
    resource::handle_batch_release(carrying_socket);

    resource::handle_batch* empty = resource::create_handle_batch(1);
    ASSERT_NOT_NULL(empty);
    EXPECT_EQ(send_with(obj_a, "x", 1, empty), static_cast<ssize_t>(resource::ERR_INVAL));
    resource::handle_batch_release(empty);

    EXPECT_NULL(resource::create_handle_batch(0));
    EXPECT_NULL(resource::create_handle_batch(resource::MAX_PASSED_HANDLES + 1));

    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

TEST(socket_test, handles_queued_to_a_socket_go_when_that_socket_closes) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    EXPECT_EQ(send_with(obj_a, "x", 1, batch_of(passenger)), static_cast<ssize_t>(1));
    resource::resource_release(passenger);

    // Nothing can receive the handles once their socket is gone, however long the sender stays
    uint32_t closes = g_passenger_closes.load_relaxed();
    resource::resource_release(obj_b);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);

    resource::resource_release(obj_a);
}

TEST(socket_test, handles_outlive_the_socket_that_sent_them) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    EXPECT_EQ(send_with(obj_a, "x", 1, batch_of(passenger)), static_cast<ssize_t>(1));
    resource::resource_release(obj_a);

    char c = 0;
    resource::handle_batch* batch = nullptr;
    EXPECT_EQ(receive_with(obj_b, &c, 1, &batch), static_cast<ssize_t>(1));
    EXPECT_TRUE(batch && batch->entries[0].obj == passenger);
    resource::handle_batch_release(batch);

    resource::resource_release(passenger);
    resource::resource_release(obj_b);
}

// A sender keeps passing batches while its receiver closes, and each batch must be dropped exactly once
constexpr uint32_t SENDS_BEFORE_CLOSE = 64;

struct closing_race_run {
    resource::resource_object* obj;
    resource::resource_object* passenger;
    ssize_t result;
    sync::atomic<uint32_t> sends;
    sync::atomic<uint32_t> done;
};

static closing_race_run g_closing_race;

static void run_sender_until_broken(void* arg) {
    closing_race_run& run = *static_cast<closing_race_run*>(arg);

    while (true) {
        resource::handle_batch* batch = batch_of(run.passenger);
        run.result = send_with(run.obj, "x", 1, batch, net::inet::MSG_NOSIGNAL);
        if (run.result <= 0) {
            resource::handle_batch_release(batch);
            break;
        }

        run.sends.fetch_add_relaxed(1);
    }

    run.done.store_release(1);
    sched::exit(0);
}

TEST(socket_test, a_receiver_closing_under_a_busy_sender_drops_every_batch_once) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    closing_race_run& run = g_closing_race;
    run.obj = obj_a;
    run.passenger = passenger;
    run.result = 0;
    run.sends.store_relaxed(0);
    run.done.store_relaxed(0);

    uint32_t closes = g_passenger_closes.load_relaxed();
    sched::task* sender = test_helpers::start_pinned_task(run_sender_until_broken, &run, "sender_until_broken");
    ASSERT_NOT_NULL(sender);
    EXPECT_TRUE(test_helpers::spin_wait_ge(run.sends, SENDS_BEFORE_CLOSE));

    resource::resource_release(obj_b);
    EXPECT_TRUE(test_helpers::spin_wait(run.done));
    EXPECT_EQ(run.result, static_cast<ssize_t>(resource::ERR_PIPE));

    // Queued or refused, every batch let go of its reference once, so only the test's is left
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes);
    resource::resource_release(passenger);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);

    test_helpers::unpin(sender);
    resource::resource_release(obj_a);
}

struct unix_address {
    uint16_t family;
    char path[socket::UNIX_PATH_MAX];
};

constexpr uint16_t UNIX_ADDRESS_FAMILY = 1;
constexpr int32_t LISTEN_BACKLOG = 1;
static const char PENDING_HANDLES_PATH[] = "/pending_handles.sock";

TEST(socket_test, closing_a_listener_frees_the_handles_of_its_pending_connections) {
    ASSERT_TRUE(cpu::irqs_enabled());

    resource::resource_object* listening = nullptr;
    resource::resource_object* client = nullptr;
    ASSERT_EQ(socket::create_unbound_socket(&listening), resource::OK);
    ASSERT_EQ(socket::create_unbound_socket(&client), resource::OK);

    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    unix_address address = {};
    address.family = UNIX_ADDRESS_FAMILY;
    string::memcpy(address.path, PENDING_HANDLES_PATH, sizeof(PENDING_HANDLES_PATH));

    const resource::socket_ops* ops = listening->ops->socket;
    ASSERT_EQ(ops->bind(listening, &address, sizeof(address)), resource::OK);
    ASSERT_EQ(ops->listen(listening, LISTEN_BACKLOG), resource::OK);
    ASSERT_EQ(ops->connect(client, &address, sizeof(address), false), resource::OK);

    // Once the client is gone, only the connection waiting in the accept queue holds the batch
    EXPECT_EQ(send_with(client, "x", 1, batch_of(passenger)), static_cast<ssize_t>(1));
    resource::resource_release(passenger);
    resource::resource_release(client);

    uint32_t closes = g_passenger_closes.load_relaxed();
    uint32_t closes_under_spinlock = g_passenger_closes_under_spinlock.load_relaxed();
    resource::resource_release(listening);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);
    EXPECT_EQ(g_passenger_closes_under_spinlock.load_relaxed(), closes_under_spinlock);

    EXPECT_EQ(fs::unlink(PENDING_HANDLES_PATH), fs::OK);
}

TEST(socket_test, a_batch_rides_on_the_first_stretch_that_fits) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    // The room one read frees takes the start of the send and the batch with it
    size_t filled = fill_stream(obj_a);
    char freed[10];
    ASSERT_TRUE(filled > sizeof(freed));
    EXPECT_EQ(obj_b->ops->read(obj_b, freed, sizeof(freed), fs::O_NONBLOCK), static_cast<ssize_t>(sizeof(freed)));
    EXPECT_EQ(send_with(obj_a, "0123456789abcdef", 16, batch_of(passenger)), static_cast<ssize_t>(sizeof(freed)));

    char* buf = reinterpret_cast<char*>(g_blocking_send_bytes);
    size_t received = 0;
    ssize_t n = 0;
    resource::handle_batch* batch = nullptr;
    while (!batch) {
        n = receive_with(obj_b, buf, sizeof(g_blocking_send_bytes), &batch);
        if (n <= 0) {
            break;
        }

        received += static_cast<size_t>(n);
    }

    // The receive that took the batch ended with exactly the bytes it rode on
    EXPECT_EQ(received, filled);
    EXPECT_TRUE(batch && batch->entries[0].obj == passenger);
    ASSERT_TRUE(n >= static_cast<ssize_t>(sizeof(freed)));
    EXPECT_EQ(string::memcmp(buf + n - sizeof(freed), "0123456789", sizeof(freed)), 0);
    resource::handle_batch_release(batch);

    resource::handle_batch* none = nullptr;
    EXPECT_EQ(receive_with(obj_b, buf, 16, &none), static_cast<ssize_t>(resource::ERR_AGAIN));
    EXPECT_NULL(none);

    resource::resource_release(passenger);
    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

// Two senders pass their own object with every send, and each batch must arrive with its own bytes
constexpr uint32_t BATCH_SENDS = 200;
constexpr size_t BATCH_SEND_LEN = 8;

struct batch_sender_run {
    resource::resource_object* obj;
    resource::resource_object* passenger;
    char letter;
    uint32_t failures;
    sync::atomic<uint32_t> done;
};

static batch_sender_run g_batch_senders[2];

static void run_batch_sender(void* arg) {
    batch_sender_run& run = *static_cast<batch_sender_run*>(arg);
    char bytes[BATCH_SEND_LEN];
    string::memset(bytes, run.letter, sizeof(bytes));

    for (uint32_t i = 0; i < BATCH_SENDS; i++) {
        resource::handle_batch* batch = batch_of(run.passenger);
        ssize_t n = send_with(run.obj, bytes, sizeof(bytes), batch, 0);
        if (n <= 0) {
            resource::handle_batch_release(batch);
        }

        if (n != static_cast<ssize_t>(sizeof(bytes))) {
            run.failures++;
        }
    }

    run.done.store_release(1);
    sched::exit(0);
}

TEST(socket_test, concurrent_senders_keep_each_batch_on_its_own_bytes) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);

    sched::task* senders[2] = {};
    for (uint32_t i = 0; i < 2; i++) {
        batch_sender_run& run = g_batch_senders[i];
        run.passenger = make_passenger();
        ASSERT_NOT_NULL(run.passenger);

        run.obj = obj_a;
        run.letter = static_cast<char>('A' + i);
        run.failures = 0;
        run.done.store_relaxed(0);
        senders[i] = test_helpers::start_pinned_task(run_batch_sender, &run, "batch_sender");
        ASSERT_NOT_NULL(senders[i]);
    }

    EXPECT_TRUE(test_helpers::spin_wait(g_batch_senders[0].done));
    EXPECT_TRUE(test_helpers::spin_wait(g_batch_senders[1].done));
    EXPECT_EQ(g_batch_senders[0].failures + g_batch_senders[1].failures, 0u);

    uint32_t matched = 0;
    uint32_t mismatched = 0;
    char buf[64];
    while (true) {
        resource::handle_batch* batch = nullptr;
        ssize_t n = receive_with(obj_b, buf, sizeof(buf), &batch);
        if (n <= 0) {
            break;
        }

        uint32_t sender = static_cast<uint32_t>(buf[0] - 'A');
        bool intact = n == static_cast<ssize_t>(BATCH_SEND_LEN) && sender < 2 && batch &&
                      batch->entries[0].obj == g_batch_senders[sender].passenger;
        for (ssize_t i = 0; intact && i < n; i++) {
            intact = buf[i] == buf[0];
        }

        if (intact) {
            matched++;
        } else {
            mismatched++;
        }

        resource::handle_batch_release(batch);
    }

    EXPECT_EQ(matched, 2 * BATCH_SENDS);
    EXPECT_EQ(mismatched, 0u);

    for (uint32_t i = 0; i < 2; i++) {
        test_helpers::unpin(senders[i]);
        resource::resource_release(g_batch_senders[i].passenger);
    }

    resource::resource_release(obj_a);
    resource::resource_release(obj_b);
}

// Two receivers outside the receive lock race over stretches that each carry a batch, taking each exactly once
constexpr uint32_t RACED_SENDS = 300;

struct batch_receiver_run {
    resource::resource_object* obj;
    resource::resource_object* passenger;
    uint32_t taken;
    uint32_t foreign;
    sync::atomic<uint32_t> done;
};

static batch_receiver_run g_batch_receivers[2];

static void run_batch_receiver(void* arg) {
    batch_receiver_run& run = *static_cast<batch_receiver_run*>(arg);
    char buf[3];

    while (true) {
        resource::handle_batch* batch = nullptr;
        ssize_t n = receive_with(run.obj, buf, sizeof(buf), &batch, 0);
        if (n <= 0) {
            break;
        }

        if (batch) {
            run.taken++;
            if (batch->entries[0].obj != run.passenger) {
                run.foreign++;
            }
        }

        resource::handle_batch_release(batch);
    }

    run.done.store_release(1);
    sched::exit(0);
}

TEST(socket_test, receivers_outside_the_receive_lock_take_each_batch_once) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::resource_object* passenger = make_passenger();
    ASSERT_NOT_NULL(passenger);

    sched::task* receivers[2] = {};
    for (uint32_t i = 0; i < 2; i++) {
        batch_receiver_run& run = g_batch_receivers[i];
        run.obj = obj_b;
        run.passenger = passenger;
        run.taken = 0;
        run.foreign = 0;
        run.done.store_relaxed(0);

        receivers[i] = test_helpers::start_pinned_task(run_batch_receiver, &run, "batch_receiver");
        ASSERT_NOT_NULL(receivers[i]);
    }

    uint32_t sent = 0;
    for (uint32_t i = 0; i < RACED_SENDS; i++) {
        resource::handle_batch* batch = batch_of(passenger);
        if (send_with(obj_a, "abcd", 4, batch) == 4) {
            sent++;
        } else {
            resource::handle_batch_release(batch);
        }
    }

    // Closing the sending side ends both receivers once they drain the stream
    resource::resource_release(obj_a);
    EXPECT_TRUE(test_helpers::spin_wait(g_batch_receivers[0].done));
    EXPECT_TRUE(test_helpers::spin_wait(g_batch_receivers[1].done));

    EXPECT_EQ(sent, RACED_SENDS);
    EXPECT_EQ(g_batch_receivers[0].taken + g_batch_receivers[1].taken, RACED_SENDS);
    EXPECT_EQ(g_batch_receivers[0].foreign + g_batch_receivers[1].foreign, 0u);

    // Every batch let go of its reference, so the test's is the last
    uint32_t closes = g_passenger_closes.load_relaxed();
    resource::resource_release(passenger);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);

    for (uint32_t i = 0; i < 2; i++) {
        test_helpers::unpin(receivers[i]);
    }

    resource::resource_release(obj_b);
}

TEST(socket_test, a_plain_receive_call_ends_where_handles_came_and_drops_them) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(socket::create_socket_pair(&obj_a, &obj_b), resource::OK);
    resource::handle_t reader = -1;
    ASSERT_EQ(resource::alloc_handle(task->handles, obj_b, resource::resource_type::SOCKET, resource::RIGHT_READ,
                                     &reader), resource::HANDLE_OK);
    resource::resource_release(obj_b);

    resource::resource_object* dropped = make_passenger();
    resource::resource_object* discarded = make_passenger();
    ASSERT_NOT_NULL(dropped);
    ASSERT_NOT_NULL(discarded);

    EXPECT_EQ(send_with(obj_a, "ab", 2, batch_of(dropped)), static_cast<ssize_t>(2));
    EXPECT_EQ(send_with(obj_a, "cd", 2, batch_of(discarded)), static_cast<ssize_t>(2));
    EXPECT_EQ(send_with(obj_a, "ef", 2, nullptr), static_cast<ssize_t>(2));
    resource::resource_release(dropped);
    resource::resource_release(discarded);

    test_helpers::user_page page;
    ASSERT_TRUE(page.ready());
    uint64_t wait_all = net::inet::MSG_WAITALL | net::inet::MSG_DONTWAIT;
    uint32_t closes = g_passenger_closes.load_relaxed();
    int64_t got = 0;

    // Even a receive told to wait for all 16 bytes ends where the handles came, dropping them
    {
        test_helpers::user_space_scope scope(page.ctx);
        got = sys_recvfrom(static_cast<uint64_t>(reader), page.addr, 16, wait_all, 0, 0);
    }
    EXPECT_EQ(got, static_cast<int64_t>(2));
    EXPECT_EQ(string::memcmp(page.at<char>(0), "ab", 2), 0);
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 1);

    {
        test_helpers::user_space_scope scope(page.ctx);
        got = sys_recvfrom(static_cast<uint64_t>(reader), page.addr, 16, wait_all | net::inet::MSG_TRUNC, 0, 0);
    }
    EXPECT_EQ(got, static_cast<int64_t>(2));
    EXPECT_EQ(g_passenger_closes.load_relaxed(), closes + 2);

    {
        test_helpers::user_space_scope scope(page.ctx);
        got = sys_recvfrom(static_cast<uint64_t>(reader), page.addr, 16, net::inet::MSG_DONTWAIT, 0, 0);
    }
    EXPECT_EQ(got, static_cast<int64_t>(2));
    EXPECT_EQ(string::memcmp(page.at<char>(0), "ef", 2), 0);

    EXPECT_EQ(resource::close(task, reader), resource::OK);
    resource::resource_release(obj_a);
}
