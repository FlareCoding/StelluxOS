#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/tcp/socket.h"
#include "net/tcp/conn.h"
#include "net/net.h"

TEST_SUITE(tcp_socket);

using namespace net;
using namespace net::tcp;

// lo0 owns 127.0.0.1 in every test image, TEST-NET-1 belongs to nobody
static const ipv4::ipv4_addr g_loopback = {{127, 0, 0, 1}};
static const ipv4::ipv4_addr g_foreign  = {{192, 0, 2, 1}};

TEST(tcp_socket, opens_unbound_and_closes) {
    tcp_socket* sock = socket_open();
    ASSERT_NOT_NULL(sock);

    EXPECT_FALSE(sock->bound);
    EXPECT_EQ(sock->local.port, 0);
    EXPECT_FALSE(sock->listener);
    EXPECT_FALSE(sock->conn);

    socket_close(sock);
}

TEST(tcp_socket, binds_the_wildcard_and_an_owned_address_only) {
    tcp_socket* any = socket_open();
    tcp_socket* owned = socket_open();
    tcp_socket* foreign = socket_open();

    EXPECT_EQ(socket_bind(any, ipv4::UNSPECIFIED_ADDR, 7000), OK);
    EXPECT_TRUE(any->bound);
    EXPECT_EQ(any->local.port, 7000);

    EXPECT_EQ(socket_bind(owned, g_loopback, 7001), OK);
    EXPECT_TRUE(owned->local.addr == g_loopback);

    EXPECT_EQ(socket_bind(foreign, g_foreign, 7002), ERR_NOT_LOCAL);
    EXPECT_FALSE(foreign->bound);

    socket_close(any);
    socket_close(owned);
    socket_close(foreign);
}

TEST(tcp_socket, binds_only_once) {
    tcp_socket* sock = socket_open();
    ASSERT_EQ(socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 7000), OK);

    EXPECT_EQ(socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 7001), ERR_INVALID);
    EXPECT_EQ(sock->local.port, 7000);

    socket_close(sock);
}

TEST(tcp_socket, port_zero_takes_an_ephemeral_port_that_others_then_avoid) {
    tcp_socket* sock = socket_open();
    ASSERT_EQ(socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 0), OK);

    EXPECT_TRUE(sock->local.port >= EPHEMERAL_PORT_MIN);
    EXPECT_TRUE(is_local_port_taken(sock->local.port));
    EXPECT_TRUE(is_socket_port(sock->local.port));

    uint16_t next = 0;
    ASSERT_EQ(take_ephemeral_port(&next), OK);
    EXPECT_TRUE(next != sock->local.port);

    uint16_t port = sock->local.port;
    socket_close(sock);
    EXPECT_FALSE(is_local_port_taken(port));
}

TEST(tcp_socket, bound_sockets_share_a_port_only_with_reuseaddr_across_addresses) {
    tcp_socket* any = socket_open();
    tcp_socket* again = socket_open();
    tcp_socket* specific = socket_open();
    ASSERT_EQ(socket_bind(any, ipv4::UNSPECIFIED_ADDR, 7000), OK);

    EXPECT_EQ(socket_bind(again, ipv4::UNSPECIFIED_ADDR, 7000), ERR_IN_USE);
    EXPECT_EQ(socket_bind(specific, g_loopback, 7000), ERR_IN_USE);

    tcp_socket* shared_any = socket_open();
    tcp_socket* shared_specific = socket_open();
    tcp_socket* shared_twice = socket_open();
    shared_any->local.reuseaddr = true;
    shared_specific->local.reuseaddr = true;
    shared_twice->local.reuseaddr = true;
    EXPECT_EQ(socket_bind(shared_any, ipv4::UNSPECIFIED_ADDR, 7001), OK);
    EXPECT_EQ(socket_bind(shared_specific, g_loopback, 7001), OK);
    EXPECT_EQ(socket_bind(shared_twice, g_loopback, 7001), ERR_IN_USE);

    tcp_socket* all[] = {any, again, specific, shared_any, shared_specific, shared_twice};
    for (tcp_socket* sock : all) {
        socket_close(sock);
    }
}

TEST(tcp_socket, a_listener_blocks_a_conflicting_bind) {
    tcp_listener* listener = alloc_listener(endpoint{ipv4::UNSPECIFIED_ADDR, 7000, nullptr, false});
    ASSERT_EQ(listener_insert(listener), OK);

    tcp_socket* sock = socket_open();
    EXPECT_EQ(socket_bind(sock, g_loopback, 7000), ERR_IN_USE);
    EXPECT_EQ(socket_bind(sock, g_loopback, 7001), OK);

    socket_close(sock);
    EXPECT_EQ(listener_remove(listener), OK);
    if (listener->release()) {
        tcp_listener::ref_destroy(listener);
    }
}

TEST(tcp_socket, closing_frees_the_port_for_the_next_bind) {
    tcp_socket* first = socket_open();
    ASSERT_EQ(socket_bind(first, ipv4::UNSPECIFIED_ADDR, 7000), OK);
    socket_close(first);

    tcp_socket* second = socket_open();
    EXPECT_EQ(socket_bind(second, ipv4::UNSPECIFIED_ADDR, 7000), OK);
    socket_close(second);
}
