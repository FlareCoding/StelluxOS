#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"
#include "net/tcp/socket.h"
#include "net/tcp/conn.h"
#include "net/tcp/info.h"
#include "net/net.h"
#include "net/inet.h"
#include "resource/socket_ops.h"
#include "fs/fstypes.h"
#include "sync/poll.h"
#include "sched/sched.h"
#include "clock/clock.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"
#include "helpers.h"

TEST_SUITE(tcp_socket);

using namespace net;
using namespace net::tcp;

constexpr uint64_t SECOND_NS = 1000000000ULL;

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

    peer establish(linked_peer& lp, uint32_t* iss) const {
        EXPECT_EQ(connect(lp.remote, true), resource::ERR_INPROGRESS);
        peer remote = replying_to(lp, 0);
        *iss = ntohl(sent_tcp(lp.link, 0)->seq);
        EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, *iss + 1)), OK);
        lp.link.clear_frames();
        return remote;
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
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_HUP | sync::POLL_ERR);
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

TEST(tcp_socket, read_returns_queued_bytes_then_would_block) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    peer remote = stream_socket::replying_to(lp, 0);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_OUT);

    uint8_t buf[16] = {};
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), fs::O_NONBLOCK), resource::ERR_AGAIN);

    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001, iss + 1, {}, "hello", 5)), OK);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_OUT);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, 2, fs::O_NONBLOCK), 2);
    EXPECT_EQ(buf[0], 'h');
    EXPECT_EQ(buf[1], 'e');
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), fs::O_NONBLOCK), 3);
    EXPECT_EQ(buf[0], 'l');
    EXPECT_EQ(buf[2], 'o');
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_OUT);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), fs::O_NONBLOCK), resource::ERR_AGAIN);
    EXPECT_EQ(sock.impl()->conn->rcv_wnd, RCV_WND_INITIAL);

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, a_read_that_frees_a_chunk_announces_the_room) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    peer remote = stream_socket::replying_to(lp, 0);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);

    static uint8_t payload[CHUNK_PAYLOAD / 2];
    size_t segments = RCV_WND_INITIAL / sizeof(payload);
    for (size_t i = 0; i < segments; i++) {
        uint32_t seq = 7001 + static_cast<uint32_t>(i * sizeof(payload));
        EXPECT_EQ(input(remote.segment(FLAG_ACK, seq, iss + 1, {}, payload, sizeof(payload))), OK);
    }

    tcp_conn* conn = sock.impl()->conn.ptr();
    EXPECT_EQ(conn->rcv_wnd, 0u);
    lp.link.clear_frames();

    static uint8_t buf[CHUNK_PAYLOAD / 2];
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), static_cast<ssize_t>(sizeof(buf)));
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_EQ(conn->rcv_wnd, 0u);

    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), static_cast<ssize_t>(sizeof(buf)));
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_ACK);
    EXPECT_EQ(ntohs(sent_tcp(lp.link, 0)->window), CHUNK_PAYLOAD);
    EXPECT_EQ(conn->rcv_wnd, CHUNK_PAYLOAD);

    lp.link.clear_frames();
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), static_cast<ssize_t>(sizeof(buf)));
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), static_cast<ssize_t>(sizeof(buf)));
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(ntohs(sent_tcp(lp.link, 0)->window), 2 * CHUNK_PAYLOAD);

    abort_connection(conn);
}

TEST(tcp_socket, read_hands_out_the_bytes_before_the_fin_then_end_of_file) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    peer remote = stream_socket::replying_to(lp, 0);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
    EXPECT_EQ(input(remote.segment(FLAG_ACK | FLAG_FIN, 7001, iss + 1, {}, "bye", 3)), OK);

    uint8_t buf[16] = {};
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_OUT);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), 3);
    EXPECT_EQ(buf[2], 'e');
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), 0);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), 0);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_OUT);

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, read_hands_out_the_bytes_before_a_reset_then_the_error) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    peer remote = stream_socket::replying_to(lp, 0);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001, iss + 1, {}, "data", 4)), OK);
    EXPECT_EQ(input(remote.rst(7005)), OK);

    uint8_t buf[16] = {};
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_HUP | sync::POLL_ERR);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), 4);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), resource::ERR_CONNRESET);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), 0);
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
    uint32_t iss = 0;
    {
        stream_socket sock;
        sock.impl()->local.iface = &lp.link;

        EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
        peer remote = stream_socket::replying_to(lp, 0);
        iss = ntohl(sent_tcp(lp.link, 0)->seq);
        EXPECT_EQ(input(remote.segment(FLAG_SYN | FLAG_ACK, 7000, iss + 1)), OK);
        EXPECT_EQ(input(remote.segment(FLAG_FIN | FLAG_ACK, 7001, iss + 1)), OK);

        uint8_t byte = 'z';
        EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_OUT);
        EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, 0), 0);
        lp.link.clear_frames();
        EXPECT_EQ(sock.obj->ops->write(sock.obj, &byte, 1, 0), 1);
        ASSERT_EQ(lp.link.frames_sent(), 1u);
        EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_PSH | FLAG_ACK);
        EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), iss + 1);
        conn = sock.impl()->conn;
        lp.link.clear_frames();
    }

    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_FIN | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), iss + 2);
    EXPECT_EQ(conn->state, tcp_state::last_ack);

    abort_connection(conn.ptr());
}

TEST(tcp_socket, shutting_down_writes_sends_the_fin_and_later_writes_break) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    peer remote = sock.establish(lp, &iss);

    EXPECT_EQ(sock.ops()->shutdown(sock.obj, resource::SHUT_WR), resource::OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_FIN | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), iss + 1);
    EXPECT_EQ(sock.impl()->conn->state, tcp_state::fin_wait_1);
    EXPECT_FALSE(sock.impl()->conn->orphaned);

    uint8_t byte = 'x';
    EXPECT_EQ(sock.obj->ops->write(sock.obj, &byte, 1, 0), resource::ERR_PIPE);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), 0u);
    EXPECT_EQ(sock.ops()->shutdown(sock.obj, resource::SHUT_WR), resource::OK);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001, iss + 2, {}, "hi", 2)), OK);
    EXPECT_EQ(sock.impl()->conn->state, tcp_state::fin_wait_2);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN);
    uint8_t buf[4] = {};
    EXPECT_EQ(sock.obj->ops->read(sock.obj, buf, sizeof(buf), 0), 2);
    EXPECT_EQ(buf[1], 'i');

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, shutting_down_reads_ends_them_at_once_and_sends_nothing) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    (void)sock.establish(lp, &iss);

    EXPECT_EQ(sock.ops()->shutdown(sock.obj, resource::SHUT_RD), resource::OK);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
    EXPECT_EQ(sock.impl()->conn->state, tcp_state::established);

    uint8_t byte = 0;
    EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, fs::O_NONBLOCK), 0);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, 0), 0);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_OUT);

    byte = 'w';
    EXPECT_EQ(sock.obj->ops->write(sock.obj, &byte, 1, 0), 1);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_PSH | FLAG_ACK);

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, shutdown_needs_an_established_connection) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    EXPECT_EQ(sock.ops()->shutdown(sock.obj, resource::SHUT_RDWR), resource::ERR_NOTCONN);

    EXPECT_EQ(sock.connect(lp.remote, true), resource::ERR_INPROGRESS);
    EXPECT_EQ(sock.ops()->shutdown(sock.obj, resource::SHUT_WR), resource::ERR_NOTCONN);
    EXPECT_EQ(sock.impl()->conn->state, tcp_state::syn_sent);

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, linger_is_kept_and_read_back) {
    stream_socket sock;

    inet::linger set = {1, 7};
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_LINGER, &set, sizeof(set)), resource::OK);
    EXPECT_TRUE(sock.impl()->linger);
    EXPECT_EQ(sock.impl()->linger_seconds, 7u);

    inet::linger got = {};
    size_t len = sizeof(got);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_LINGER, &got, &len), resource::OK);
    EXPECT_EQ(got.on, 1);
    EXPECT_EQ(got.seconds, 7);
    EXPECT_EQ(len, sizeof(got));

    len = sizeof(int32_t);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_LINGER, &got, &len), resource::ERR_INVAL);
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_LINGER, &set, sizeof(int32_t)), resource::ERR_INVAL);

    set = {0, 3};
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_LINGER, &set, sizeof(set)), resource::OK);
    EXPECT_FALSE(sock.impl()->linger);
}

TEST(tcp_socket, closing_with_a_zero_linger_resets_at_once) {
    linked_peer lp;
    rc::strong_ref<tcp_conn> conn;
    uint32_t iss = 0;
    {
        stream_socket sock;
        sock.impl()->local.iface = &lp.link;
        (void)sock.establish(lp, &iss);

        inet::linger set = {1, 0};
        EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_LINGER, &set, sizeof(set)), resource::OK);
        conn = sock.impl()->conn;
    }

    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->seq), iss + 1);
    EXPECT_EQ(conn->state, tcp_state::closed);
    EXPECT_EQ(record_count(record_kind::connection), 0u);
}

// The wait runs in its own task: the runner's stack is privileged, and the
// poll entry the wait leaves on the connection must be reachable lowered
struct fin_wait_run {
    tcp_conn*              conn;
    uint64_t               timeout_ns;
    uint64_t               elapsed_ns;
    bool                   acknowledged;
    sync::atomic<uint32_t> done;
};

static fin_wait_run g_fin_wait;

static void wait_for_fin_ack(void*) {
    uint64_t started = clock::now_ns();
    RUN_ELEVATED(g_fin_wait.acknowledged = wait_fin_acknowledged(g_fin_wait.conn, g_fin_wait.timeout_ns));
    g_fin_wait.elapsed_ns = clock::now_ns() - started;
    g_fin_wait.done.store_release(1);
    sched::exit(0);
}

static void start_fin_wait(tcp_conn* conn, uint64_t timeout_ns) {
    g_fin_wait.conn = conn;
    g_fin_wait.timeout_ns = timeout_ns;
    g_fin_wait.elapsed_ns = 0;
    g_fin_wait.acknowledged = false;
    g_fin_wait.done.store_relaxed(0);

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(wait_for_fin_ack, nullptr, "tcp_fin_wait");
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });
}

TEST(tcp_socket, waiting_for_the_fin_to_be_acknowledged_ends_when_the_ack_arrives) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    peer remote = sock.establish(lp, &iss);

    rc::strong_ref<tcp_conn> conn = sock.impl()->conn;
    close_connection(conn.ptr());
    EXPECT_EQ(conn->state, tcp_state::fin_wait_1);

    start_fin_wait(conn.ptr(), 5 * SECOND_NS);
    RUN_ELEVATED(sched::sleep_ms(30));
    EXPECT_EQ(g_fin_wait.done.load_acquire(), 0u);

    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001, iss + 2)), OK);
    EXPECT_TRUE(test_helpers::spin_wait(g_fin_wait.done));

    EXPECT_TRUE(g_fin_wait.acknowledged);
    EXPECT_EQ(conn->state, tcp_state::fin_wait_2);
    EXPECT_TRUE(g_fin_wait.elapsed_ns < 5 * SECOND_NS);

    abort_connection(conn.ptr());
}

TEST(tcp_socket, waiting_for_the_fin_to_be_acknowledged_gives_up_when_the_time_passes) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    (void)sock.establish(lp, &iss);

    rc::strong_ref<tcp_conn> conn = sock.impl()->conn;
    close_connection(conn.ptr());

    start_fin_wait(conn.ptr(), SECOND_NS / 20);
    EXPECT_TRUE(test_helpers::spin_wait(g_fin_wait.done));

    EXPECT_FALSE(g_fin_wait.acknowledged);
    EXPECT_TRUE(g_fin_wait.elapsed_ns >= SECOND_NS / 20);
    EXPECT_EQ(conn->state, tcp_state::fin_wait_1);

    abort_connection(conn.ptr());
}

TEST(tcp_socket, send_and_recv_honor_the_message_flags) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    peer remote = sock.establish(lp, &iss);

    EXPECT_EQ(sock.ops()->sendto(sock.obj, "hi", 2, 0, nullptr, 0), 2);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(sent_tcp(lp.link, 0)->flags, FLAG_PSH | FLAG_ACK);

    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001, iss + 3, {}, "hello", 5)), OK);

    uint8_t buf[16] = {};
    inet::sockaddr_in from = {};
    size_t from_len = sizeof(from);
    EXPECT_EQ(sock.ops()->recvfrom(sock.obj, buf, sizeof(buf), inet::MSG_PEEK, &from, &from_len), 5);
    EXPECT_EQ(buf[0], 'h');
    EXPECT_EQ(sock.impl()->conn->rcv_queue.size(), 5u);
    EXPECT_TRUE(from.addr == remote.addr);
    EXPECT_EQ(ntohs(from.port), remote.port);

    EXPECT_EQ(sock.ops()->recvfrom(sock.obj, buf, sizeof(buf), inet::MSG_WAITALL | inet::MSG_DONTWAIT, nullptr, nullptr), 5);
    EXPECT_EQ(buf[4], 'o');
    EXPECT_EQ(sock.ops()->recvfrom(sock.obj, buf, sizeof(buf), inet::MSG_DONTWAIT, nullptr, nullptr), resource::ERR_AGAIN);
    EXPECT_EQ(sock.ops()->recvfrom(sock.obj, buf, sizeof(buf), inet::MSG_OOB | inet::MSG_DONTWAIT, nullptr, nullptr),
              resource::ERR_UNSUP);
    EXPECT_EQ(sock.ops()->sendto(sock.obj, "x", 1, inet::MSG_OOB, nullptr, 0), resource::ERR_UNSUP);

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, sendto_with_another_address_is_refused_as_connected) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    peer remote = sock.establish(lp, &iss);

    inet::sockaddr_in same = {inet::AF_INET, htons(remote.port), remote.addr, {}};
    inet::sockaddr_in other = {inet::AF_INET, htons(static_cast<uint16_t>(remote.port + 1)), remote.addr, {}};
    EXPECT_EQ(sock.ops()->sendto(sock.obj, "x", 1, 0, &other, sizeof(other)), resource::ERR_ISCONN);
    EXPECT_EQ(sock.ops()->sendto(sock.obj, "x", 1, 0, &same, sizeof(same)), 1);
    EXPECT_EQ(lp.link.frames_sent(), 1u);

    abort_connection(sock.impl()->conn.ptr());
}

TEST(tcp_socket, writes_after_our_fin_break_with_or_without_the_signal) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    (void)sock.establish(lp, &iss);

    EXPECT_EQ(sock.ops()->shutdown(sock.obj, resource::SHUT_WR), resource::OK);
    EXPECT_EQ(sock.ops()->sendto(sock.obj, "x", 1, inet::MSG_NOSIGNAL, nullptr, 0), resource::ERR_PIPE);
    EXPECT_EQ(sock.ops()->sendto(sock.obj, "x", 1, 0, nullptr, 0), resource::ERR_PIPE);
    EXPECT_EQ(sock.obj->ops->write(sock.obj, "x", 1, 0), resource::ERR_PIPE);

    abort_connection(sock.impl()->conn.ptr());
}

// The whole-count read blocks in an elevated task of its own while the runner feeds it
struct whole_read_run {
    resource::resource_object* obj;
    uint8_t                    buf[10];
    ssize_t                    result;
    sync::atomic<uint32_t>     done;
};

static whole_read_run g_whole_read;

static void read_whole(void*) {
    g_whole_read.result = g_whole_read.obj->ops->socket->recvfrom(
        g_whole_read.obj, g_whole_read.buf, sizeof(g_whole_read.buf), inet::MSG_WAITALL, nullptr, nullptr);
    g_whole_read.done.store_release(1);
    sched::exit(0);
}

TEST(tcp_socket, recv_with_waitall_waits_for_the_whole_count) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    peer remote = sock.establish(lp, &iss);

    g_whole_read.obj = sock.obj;
    g_whole_read.result = 0;
    g_whole_read.done.store_relaxed(0);
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(read_whole, nullptr, "tcp_waitall", sched::TASK_FLAG_ELEVATED);
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });

    RUN_ELEVATED(sched::sleep_ms(20));
    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001, iss + 1, {}, "hello", 5)), OK);
    RUN_ELEVATED(sched::sleep_ms(20));
    EXPECT_EQ(g_whole_read.done.load_acquire(), 0u);

    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7006, iss + 1, {}, "world", 5)), OK);
    EXPECT_TRUE(test_helpers::spin_wait(g_whole_read.done));
    EXPECT_EQ(g_whole_read.result, 10);
    EXPECT_EQ(g_whole_read.buf[0], 'h');
    EXPECT_EQ(g_whole_read.buf[9], 'd');
    EXPECT_EQ(sock.impl()->conn->rcv_queue.size(), 0u);

    abort_connection(sock.impl()->conn.ptr());
}

// A read or write larger than the queue, blocked in an elevated task while the
// runner plays the peer through the stub link
struct big_transfer_run {
    resource::resource_object* obj;
    bool                       writing;
    ssize_t                    result;
    sync::atomic<uint32_t>     done;
};

constexpr size_t BIG_TRANSFER = SND_CHUNKS_INITIAL * CHUNK_PAYLOAD + 1000;

static big_transfer_run g_big_transfer;
static uint8_t          g_big_bytes[BIG_TRANSFER];

static void move_big_transfer(void*) {
    resource::resource_object* obj = g_big_transfer.obj;
    if (g_big_transfer.writing) {
        g_big_transfer.result = obj->ops->write(obj, g_big_bytes, sizeof(g_big_bytes), 0);
    } else {
        g_big_transfer.result = obj->ops->socket->recvfrom(obj, g_big_bytes, sizeof(g_big_bytes), inet::MSG_WAITALL,
                                                           nullptr, nullptr);
    }

    g_big_transfer.done.store_release(1);
    sched::exit(0);
}

static void start_big_transfer(resource::resource_object* obj, bool writing) {
    g_big_transfer.obj = obj;
    g_big_transfer.writing = writing;
    g_big_transfer.result = 0;
    g_big_transfer.done.store_relaxed(0);
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(move_big_transfer, nullptr, "tcp_big_move", sched::TASK_FLAG_ELEVATED);
        ASSERT_NOT_NULL(t);
        sched::enqueue(t);
    });
}

TEST(tcp_socket, a_write_larger_than_the_queue_sends_what_it_queued_before_waiting_for_room) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    peer remote = sock.establish(lp, &iss);
    tcp_conn* conn = sock.impl()->conn.ptr();

    start_big_transfer(sock.obj, true);

    // Acknowledge everything as it appears until the write returned and its last byte went out
    uint32_t acked = iss + 1;
    uint32_t end = iss + 1 + BIG_TRANSFER;
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;

    while (clock::now_ns() < deadline) {
        uint32_t snd_nxt = 0;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            snd_nxt = conn->snd_nxt;
        });

        if (snd_nxt != acked) {
            EXPECT_EQ(input(remote.ack(7001, snd_nxt)), OK);
            acked = snd_nxt;
        }

        if (g_big_transfer.done.load_acquire() && snd_nxt == end) {
            break;
        }
    }

    EXPECT_TRUE(g_big_transfer.done.load_acquire());
    EXPECT_EQ(g_big_transfer.result, static_cast<ssize_t>(BIG_TRANSFER));
    EXPECT_EQ(conn->snd_nxt, end);

    abort_connection(conn);
}

TEST(tcp_socket, a_waitall_read_larger_than_the_queue_announces_the_room_it_frees_before_waiting) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    uint32_t iss = 0;
    peer remote = sock.establish(lp, &iss);
    tcp_conn* conn = sock.impl()->conn.ptr();

    start_big_transfer(sock.obj, false);

    static uint8_t payload[1024];
    size_t window = RCV_WND_INITIAL;
    for (size_t sent = 0; sent < window; sent += sizeof(payload)) {
        size_t len = window - sent < sizeof(payload) ? window - sent : sizeof(payload);
        EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001 + static_cast<uint32_t>(sent), iss + 1, {}, payload, len)), OK);
    }

    // The peer may send the rest only once the reader has drained the queue and
    // this host has advertised the room again
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    bool reopened = false;
    while (!reopened && clock::now_ns() < deadline) {
        RUN_ELEVATED({
            sync::irq_lock_guard guard(conn->lock);
            reopened = conn->rcv_queue.size() == 0 && conn->rcv_adv - conn->rcv_nxt >= BIG_TRANSFER - window;
        });
    }

    EXPECT_TRUE(reopened);
    EXPECT_EQ(input(remote.segment(FLAG_ACK, 7001 + static_cast<uint32_t>(window), iss + 1, {}, payload,
                                   BIG_TRANSFER - window)), OK);
    EXPECT_TRUE(test_helpers::spin_wait(g_big_transfer.done));
    EXPECT_EQ(g_big_transfer.result, static_cast<ssize_t>(BIG_TRANSFER));

    abort_connection(conn);
}

TEST(tcp_socket, tcp_options_are_kept_and_reach_the_connection) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;

    int32_t value = 1;
    size_t len = sizeof(value);
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_NODELAY, &value, sizeof(value)), resource::OK);
    value = 500;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, sizeof(value)), resource::OK);
    value = 50;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, sizeof(value)), resource::ERR_INVAL);
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_TYPE, &value, sizeof(value)), resource::ERR_NOPROTOOPT);
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, 99, &value, sizeof(value)), resource::ERR_NOPROTOOPT);

    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_NODELAY, &value, &len), resource::OK);
    EXPECT_EQ(value, 1);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, &len), resource::OK);
    EXPECT_EQ(value, 500);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_TYPE, &value, &len), resource::OK);
    EXPECT_EQ(value, static_cast<int32_t>(inet::SOCK_STREAM));
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_ACCEPTCONN, &value, &len), resource::OK);
    EXPECT_EQ(value, 0);

    uint32_t iss = 0;
    (void)sock.establish(lp, &iss);
    tcp_conn* conn = sock.impl()->conn.ptr();
    EXPECT_TRUE(conn->nodelay);
    EXPECT_EQ(conn->snd_mss, 500);

    value = 300;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, sizeof(value)), resource::OK);
    EXPECT_EQ(conn->snd_mss, 300);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, &len), resource::OK);
    EXPECT_EQ(value, 300);

    // Lifting the cap gives the negotiated size back, and a cap above it changes nothing
    value = 0;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, sizeof(value)), resource::OK);
    EXPECT_EQ(conn->snd_mss, DEFAULT_MSS);
    value = 2000;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, sizeof(value)), resource::OK);
    EXPECT_EQ(conn->snd_mss, DEFAULT_MSS);
    value = 300;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_MAXSEG, &value, sizeof(value)), resource::OK);
    EXPECT_EQ(conn->snd_mss, 300);

    tcp_record record = {};
    size_t record_len = sizeof(record);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_INFO, &record, &record_len), resource::OK);
    EXPECT_EQ(record.kind, INFO_KIND_CONNECTION);
    EXPECT_EQ(record.state, static_cast<uint8_t>(tcp_state::established));
    EXPECT_EQ(record.snd_mss, 300);
    record_len = 8;
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_INFO, &record, &record_len), resource::ERR_INVAL);

    value = 0;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_QUICKACK, &value, sizeof(value)), resource::OK);
    EXPECT_EQ(conn->quick_acks, 0);
    value = 1;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_QUICKACK, &value, sizeof(value)), resource::OK);
    EXPECT_EQ(conn->quick_acks, MAX_QUICKACKS);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_QUICKACK, &value, &len), resource::OK);
    EXPECT_EQ(value, 1);

    abort_connection(conn);
}

TEST(tcp_socket, a_listeners_options_reach_the_connections_it_accepts) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    ASSERT_EQ(socket_bind(sock.impl(), ipv4::UNSPECIFIED_ADDR, lp.remote.host_port), OK);
    ASSERT_EQ(sock.ops()->listen(sock.obj, 5), resource::OK);

    int32_t value = 1;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_NODELAY, &value, sizeof(value)), resource::OK);
    size_t len = sizeof(value);
    EXPECT_EQ(sock.ops()->getsockopt(sock.obj, inet::SOL_SOCKET, inet::SO_ACCEPTCONN, &value, &len), resource::OK);
    EXPECT_EQ(value, 1);

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(lp.remote.ack(1001, iss + 1)), OK);

    resource::resource_object* child_obj = nullptr;
    ASSERT_EQ(sock.ops()->accept(sock.obj, &child_obj, nullptr, nullptr, true), resource::OK);
    tcp_socket* child = static_cast<tcp_socket*>(child_obj->impl);
    EXPECT_TRUE(child->options.nodelay);
    EXPECT_TRUE(child->conn->nodelay);

    abort_connection(child->conn.ptr());
    child_obj->ops->close(child_obj);
    heap::kfree_delete(child_obj);
}

TEST(tcp_socket, an_option_set_after_the_handshake_is_not_claimed_by_a_connection_made_before_it) {
    linked_peer lp;
    stream_socket sock;
    sock.impl()->local.iface = &lp.link;
    ASSERT_EQ(socket_bind(sock.impl(), ipv4::UNSPECIFIED_ADDR, lp.remote.host_port), OK);
    ASSERT_EQ(sock.ops()->listen(sock.obj, 5), resource::OK);

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    uint32_t iss = ntohl(sent_tcp(lp.link, 0)->seq);
    EXPECT_EQ(input(lp.remote.ack(1001, iss + 1)), OK);

    int32_t value = 1;
    EXPECT_EQ(sock.ops()->setsockopt(sock.obj, inet::IPPROTO_TCP, inet::TCP_NODELAY, &value, sizeof(value)), resource::OK);

    resource::resource_object* child_obj = nullptr;
    ASSERT_EQ(sock.ops()->accept(sock.obj, &child_obj, nullptr, nullptr, true), resource::OK);
    tcp_socket* child = static_cast<tcp_socket*>(child_obj->impl);
    size_t len = sizeof(value);
    EXPECT_EQ(child_obj->ops->socket->getsockopt(child_obj, inet::IPPROTO_TCP, inet::TCP_NODELAY, &value, &len), resource::OK);
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(child->options.nodelay);
    EXPECT_FALSE(child->conn->nodelay);

    abort_connection(child->conn.ptr());
    child_obj->ops->close(child_obj);
    heap::kfree_delete(child_obj);
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
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_HUP | sync::POLL_ERR);

    uint8_t byte = 0;
    EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, 0), resource::ERR_CONNRESET);
    EXPECT_EQ(sock.obj->ops->read(sock.obj, &byte, 1, 0), 0);
    EXPECT_EQ(sock.obj->ops->write(sock.obj, &byte, 1, 0), resource::ERR_PIPE);
    EXPECT_EQ(sock.pending_error(), resource::OK);
    EXPECT_EQ(sock.obj->ops->poll(sock.obj, nullptr), sync::POLL_IN | sync::POLL_RDHUP | sync::POLL_HUP);
}
