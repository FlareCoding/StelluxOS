#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/socket.h"
#include "net/tcp/conn.h"
#include "net/net.h"
#include "net/inet.h"
#include "resource/socket_ops.h"
#include "sync/poll.h"
#include "mm/heap.h"

TEST_SUITE(tcp_socket);

using namespace net;
using namespace net::tcp;

// lo0 owns 127.0.0.1 in every test image, TEST-NET-1 belongs to nobody
static const ipv4::ipv4_addr g_loopback = {{127, 0, 0, 1}};
static const ipv4::ipv4_addr g_foreign  = {{192, 0, 2, 1}};

// A stream socket behind its resource object, the way the syscalls reach it
struct stream_socket {
    resource::resource_object* obj;

    stream_socket() {
        obj = heap::kalloc_new<resource::resource_object>();
        obj->type = resource::resource_type::SOCKET;
        obj->ops = socket_ops();
        obj->impl = socket_open();
    }

    ~stream_socket() {
        obj->ops->close(obj);
        heap::kfree_delete(obj);
    }

    tcp_socket* impl() const { return static_cast<tcp_socket*>(obj->impl); }
    const resource::socket_ops* ops() const { return obj->ops->socket; }

    int32_t connect(const peer& remote, bool nonblock) const {
        inet::sockaddr_in addr = {inet::AF_INET, htons(remote.port), remote.addr, {}};
        return ops()->connect(obj, &addr, sizeof(addr), nonblock);
    }

    // The peer as it must answer: to the port the socket's SYN in `frame` came from
    static peer replying_to(const linked_peer& lp, size_t frame) {
        peer remote = lp.remote;
        remote.host_port = ntohs(sent_tcp(lp.link, frame)->src_port);
        return remote;
    }

    int32_t pending_error() const {
        int32_t value = -1;
        size_t len = sizeof(value);
        ops()->getsockopt(obj, inet::SOL_SOCKET, inet::SO_ERROR, &value, &len);
        return value;
    }
};

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

TEST(tcp_socket, one_connect_at_a_time_and_a_failure_reported_once_frees_the_socket) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_SYN);
    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_ALREADY);
    EXPECT_EQ(sock.ops()->listen(sock.obj, 5), resource::ERR_INVAL);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), 0u);

    // The peer refuses: poll and SO_ERROR report it, the peer name is gone
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(stream_socket::replying_to(lp, 0).segment(FLAG_RST | FLAG_ACK, 0, iss + 1)), OK);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_HUP | sync::POLL_ERR);
    EXPECT_EQ(sock.pending_error(), resource::ERR_CONNREFUSED);
    EXPECT_EQ(sock.pending_error(), resource::OK);

    uint8_t name[inet::SOCKADDR_IN_LEN];
    size_t name_len = sizeof(name);
    EXPECT_EQ(sock.ops()->getname(sock.obj, name, &name_len, true), resource::ERR_NOTCONN);

    // The attempt after a failure reports it and the one after that starts
    // fresh from the port the socket was given
    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_CONNREFUSED);
    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    EXPECT_EQ(lp.link.frames_sent(), 2u);
    EXPECT_EQ(sent_tcp(lp.link, 1)->flags, FLAG_SYN);
    EXPECT_EQ(ntohs(sent_tcp(lp.link, 1)->src_port), ntohs(sent_tcp(lp.link, 0)->src_port));
}

TEST(tcp_socket, connect_completes_and_reports_the_peer) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(stream_socket::replying_to(lp, 0).segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);

    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_OUT);
    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_ISCONN);
    EXPECT_EQ(sock.pending_error(), resource::OK);

    inet::sockaddr_in name = {};
    size_t name_len = sizeof(name);
    ASSERT_EQ(sock.ops()->getname(sock.obj, &name, &name_len, true), resource::OK);
    EXPECT_TRUE(name.addr == lp.remote.addr);
    EXPECT_EQ(ntohs(name.port), lp.remote.port);

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, closing_an_established_socket_sends_a_fin_and_orphans_the_connection) {
    linked_peer lp;
    rc::strong_ref<tcp_conn> conn;
    {
        stream_socket sock;
        sock.impl()->local.iface = &lp.link;

        EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
        uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
        EXPECT_EQ(input(stream_socket::replying_to(lp, 0).segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
        conn = sock.impl()->conn;
        lp.link.clear_frames();
    }

    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_FIN | FLAG_ACK);
    EXPECT_EQ(conn->state, tcp_state::fin_wait_1);
    EXPECT_TRUE(conn->orphaned);
    EXPECT_EQ(conn->owner, nullptr);
    EXPECT_EQ(record_count(record_kind::connection), 1u);

    abort_connection(conn.ptr());
}

TEST(tcp_socket, the_peers_fin_ends_reads_and_polls_readable_until_the_socket_closes) {
    linked_peer lp;
    rc::strong_ref<tcp_conn> conn;
    {
        stream_socket sock;
        sock.impl()->local.iface = &lp.link;

        EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
        peer remote = stream_socket::replying_to(lp, 0);
        uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
        EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
        EXPECT_EQ(input(remote.segment(FLAG_FIN | FLAG_ACK, 7001, iss + 1)), OK);

        uint8_t byte = 0;
        EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_OUT);
        EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, 0), 0);
        EXPECT_EQ(sock.obj->ops->write(sock.obj, &byte, 1, 0), resource::ERR_UNSUP);
        conn = sock.impl()->conn;
        lp.link.clear_frames();
    }

    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_FIN | FLAG_ACK);
    EXPECT_EQ(conn->state, tcp_state::last_ack);

    abort_connection(conn.ptr());
}

TEST(tcp_socket, a_reset_from_the_peer_is_reported_once_then_reads_end_and_writes_break) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    peer remote = stream_socket::replying_to(lp, 0);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
    ASSERT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_OUT);

    EXPECT_EQ(input(remote.rst(7001)), OK);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_HUP | sync::POLL_ERR);

    uint8_t byte = 0;
    EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, 0), resource::ERR_CONNRESET);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, 0), 0);
    EXPECT_EQ(sock.obj->ops->write(sock.obj, &byte, 1, 0), resource::ERR_PIPE);
    EXPECT_EQ(sock.pending_error(), resource::OK);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_HUP);
}
