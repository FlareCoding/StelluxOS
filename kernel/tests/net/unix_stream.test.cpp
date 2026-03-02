#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/unix_stream.h"
#include "common/string.h"

TEST_SUITE(unix_stream);

namespace {

void close_and_release(net::unix_stream::stream_socket* socket) {
    if (!socket) {
        return;
    }
    net::unix_stream::close(socket);
    net::unix_stream::release(socket);
}

void assert_make_path(const char* cstr, net::unix_stream::socket_path* out_path) {
    ASSERT_EQ(net::unix_stream::make_path_cstr(cstr, out_path), net::unix_stream::OK);
}

} // namespace

TEST(unix_stream, create_close_lifecycle) {
    net::unix_stream::stream_socket* socket = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(false, &socket), net::unix_stream::OK);
    ASSERT_NOT_NULL(socket);

    EXPECT_EQ(net::unix_stream::close(socket), net::unix_stream::OK);
    EXPECT_EQ(net::unix_stream::close(socket), net::unix_stream::OK);
    net::unix_stream::release(socket);
}

TEST(unix_stream, bind_rejects_duplicate_path) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_bind_duplicate", &path);

    net::unix_stream::stream_socket* a = nullptr;
    net::unix_stream::stream_socket* b = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(false, &a), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::create_socket(false, &b), net::unix_stream::OK);

    ASSERT_EQ(net::unix_stream::bind(a, path), net::unix_stream::OK);
    EXPECT_EQ(net::unix_stream::bind(b, path), net::unix_stream::ERR_ADDRINUSE);

    close_and_release(a);
    close_and_release(b);
}

TEST(unix_stream, listen_requires_bound_state) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_listen_precondition", &path);

    net::unix_stream::stream_socket* listener = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(false, &listener), net::unix_stream::OK);

    EXPECT_EQ(net::unix_stream::listen(listener, 4), net::unix_stream::ERR_INVAL);
    ASSERT_EQ(net::unix_stream::bind(listener, path), net::unix_stream::OK);
    EXPECT_EQ(net::unix_stream::listen(listener, 4), net::unix_stream::OK);

    close_and_release(listener);
}

TEST(unix_stream, connect_to_missing_listener_returns_connrefused) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_missing_listener", &path);

    net::unix_stream::stream_socket* client = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(false, &client), net::unix_stream::OK);

    EXPECT_EQ(net::unix_stream::connect(client, path), net::unix_stream::ERR_CONNREFUSED);

    close_and_release(client);
}

TEST(unix_stream, connect_accept_and_duplex_roundtrip) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_roundtrip", &path);

    net::unix_stream::stream_socket* listener = nullptr;
    net::unix_stream::stream_socket* client = nullptr;
    net::unix_stream::stream_socket* server = nullptr;

    ASSERT_EQ(net::unix_stream::create_socket(false, &listener), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::bind(listener, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::listen(listener, 2), net::unix_stream::OK);

    ASSERT_EQ(net::unix_stream::create_socket(false, &client), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::connect(client, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::accept(listener, &server), net::unix_stream::OK);
    ASSERT_NOT_NULL(server);

    const char* c2s = "hello-server";
    const char* s2c = "hello-client";
    char recv_buf[32] = {};

    ASSERT_EQ(net::unix_stream::send(client, c2s, 12), static_cast<ssize_t>(12));
    ASSERT_EQ(net::unix_stream::recv(server, recv_buf, 12), static_cast<ssize_t>(12));
    recv_buf[12] = '\0';
    EXPECT_STREQ(recv_buf, c2s);

    string::memset(recv_buf, 0, sizeof(recv_buf));
    ASSERT_EQ(net::unix_stream::send(server, s2c, 12), static_cast<ssize_t>(12));
    ASSERT_EQ(net::unix_stream::recv(client, recv_buf, 12), static_cast<ssize_t>(12));
    recv_buf[12] = '\0';
    EXPECT_STREQ(recv_buf, s2c);

    close_and_release(server);
    close_and_release(client);
    close_and_release(listener);
}

TEST(unix_stream, nonblocking_accept_and_recv_return_again) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_nonblock", &path);

    net::unix_stream::stream_socket* listener = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(true, &listener), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::bind(listener, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::listen(listener, 1), net::unix_stream::OK);

    net::unix_stream::stream_socket* accepted = nullptr;
    EXPECT_EQ(net::unix_stream::accept(listener, &accepted), net::unix_stream::ERR_AGAIN);

    net::unix_stream::stream_socket* client = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(false, &client), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::connect(client, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::accept(listener, &accepted), net::unix_stream::OK);
    ASSERT_NOT_NULL(accepted);

    ASSERT_EQ(net::unix_stream::set_nonblocking(client, true), net::unix_stream::OK);
    char byte = 0;
    EXPECT_EQ(net::unix_stream::recv(client, &byte, 1), net::unix_stream::ERR_AGAIN);

    close_and_release(accepted);
    close_and_release(client);
    close_and_release(listener);
}

TEST(unix_stream, backlog_full_then_accept_allows_next_connect) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_backlog", &path);

    net::unix_stream::stream_socket* listener = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(false, &listener), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::bind(listener, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::listen(listener, 1), net::unix_stream::OK);

    net::unix_stream::stream_socket* client_a = nullptr;
    net::unix_stream::stream_socket* client_b = nullptr;
    ASSERT_EQ(net::unix_stream::create_socket(false, &client_a), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::create_socket(true, &client_b), net::unix_stream::OK);

    ASSERT_EQ(net::unix_stream::connect(client_a, path), net::unix_stream::OK);
    EXPECT_EQ(net::unix_stream::connect(client_b, path), net::unix_stream::ERR_AGAIN);

    net::unix_stream::stream_socket* server_a = nullptr;
    ASSERT_EQ(net::unix_stream::accept(listener, &server_a), net::unix_stream::OK);
    ASSERT_NOT_NULL(server_a);

    ASSERT_EQ(net::unix_stream::connect(client_b, path), net::unix_stream::OK);
    net::unix_stream::stream_socket* server_b = nullptr;
    ASSERT_EQ(net::unix_stream::accept(listener, &server_b), net::unix_stream::OK);
    ASSERT_NOT_NULL(server_b);

    close_and_release(server_b);
    close_and_release(server_a);
    close_and_release(client_b);
    close_and_release(client_a);
    close_and_release(listener);
}

TEST(unix_stream, peer_close_yields_eof_and_pipe) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_peer_close", &path);

    net::unix_stream::stream_socket* listener = nullptr;
    net::unix_stream::stream_socket* client = nullptr;
    net::unix_stream::stream_socket* server = nullptr;

    ASSERT_EQ(net::unix_stream::create_socket(false, &listener), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::bind(listener, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::listen(listener, 1), net::unix_stream::OK);

    ASSERT_EQ(net::unix_stream::create_socket(false, &client), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::connect(client, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::accept(listener, &server), net::unix_stream::OK);

    close_and_release(server);

    char buf = 0;
    EXPECT_EQ(net::unix_stream::recv(client, &buf, 1), static_cast<ssize_t>(0));
    EXPECT_EQ(net::unix_stream::send(client, "x", 1), static_cast<ssize_t>(net::unix_stream::ERR_PIPE));

    close_and_release(client);
    close_and_release(listener);
}

TEST(unix_stream, closing_listener_closes_pending_connections) {
    net::unix_stream::socket_path path = {};
    assert_make_path("/uds_listener_close_pending", &path);

    net::unix_stream::stream_socket* listener = nullptr;
    net::unix_stream::stream_socket* client = nullptr;

    ASSERT_EQ(net::unix_stream::create_socket(false, &listener), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::bind(listener, path), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::listen(listener, 4), net::unix_stream::OK);

    ASSERT_EQ(net::unix_stream::create_socket(false, &client), net::unix_stream::OK);
    ASSERT_EQ(net::unix_stream::connect(client, path), net::unix_stream::OK);

    ASSERT_EQ(net::unix_stream::close(listener), net::unix_stream::OK);

    net::unix_stream::stream_socket* accepted = nullptr;
    EXPECT_EQ(net::unix_stream::accept(listener, &accepted), net::unix_stream::ERR_INVAL);

    char buf = 0;
    EXPECT_EQ(net::unix_stream::recv(client, &buf, 1), static_cast<ssize_t>(0));
    EXPECT_EQ(net::unix_stream::send(client, "x", 1), static_cast<ssize_t>(net::unix_stream::ERR_PIPE));

    net::unix_stream::release(listener);
    close_and_release(client);
}
