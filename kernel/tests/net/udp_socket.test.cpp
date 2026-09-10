#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/udp_socket.h"
#include "net/udp.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/net.h"

TEST_SUITE(udp_socket);

using namespace net;

static const ipv4::ipv4_addr g_peer = {{10, 0, 2, 2}};
static const ipv4::ipv4_addr g_host = {{10, 0, 2, 15}};

// A datagram as udp::input hands it to the sockets: network header marked, window at UDP
static packet* make_datagram(uint16_t dst_port) {
    packet* pkt = packet::alloc();
    if (!pkt) {
        return nullptr;
    }

    (void)pkt->reserve(eth::HEADER_LEN);

    auto* ip = reinterpret_cast<ipv4::ipv4_header*>(pkt->put(ipv4::HEADER_LEN));
    ip->set_version_ihl(ipv4::VERSION, ipv4::MIN_IHL);
    ip->proto = ipv4::PROTO_UDP;
    ip->src = g_peer;
    ip->dst = g_host;
    pkt->mark_network_header();

    auto* hdr = reinterpret_cast<udp::udp_header*>(pkt->put(udp::HEADER_LEN));
    hdr->src_port = htons(40000);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons(udp::HEADER_LEN);

    (void)pkt->pull(ipv4::HEADER_LEN);
    return pkt;
}

TEST(udp_socket, deliver_skips_unbound_socket) {
    udp::udp_socket* sock = udp::socket_open();
    ASSERT_NOT_NULL(sock);

    packet* pkt = make_datagram(0);
    ASSERT_NOT_NULL(pkt);

    EXPECT_EQ(udp::socket_deliver(pkt), ERR_NOT_FOUND);
    EXPECT_TRUE(sock->rx_queue.empty());

    packet::free(pkt);
    udp::socket_close(sock);
}

TEST(udp_socket, deliver_reaches_bound_socket) {
    udp::udp_socket* sock = udp::socket_open();
    ASSERT_NOT_NULL(sock);
    ASSERT_EQ(udp::socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 50000), OK);

    packet* pkt = make_datagram(50000);
    ASSERT_NOT_NULL(pkt);

    EXPECT_EQ(udp::socket_deliver(pkt), OK);
    EXPECT_EQ(sock->rx_queue.size(), static_cast<size_t>(1));
    EXPECT_EQ(sock->rx_queue.front(), pkt);
    EXPECT_EQ(pkt->length(), static_cast<size_t>(0));

    packet* other = make_datagram(50001);
    ASSERT_NOT_NULL(other);
    EXPECT_EQ(udp::socket_deliver(other), ERR_NOT_FOUND);
    packet::free(other);

    udp::socket_close(sock);
}

TEST(udp_socket, bind_rejects_conflicts) {
    udp::udp_socket* first = udp::socket_open();
    udp::udp_socket* second = udp::socket_open();
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    EXPECT_EQ(udp::socket_bind(first, ipv4::UNSPECIFIED_ADDR, 50002), OK);
    EXPECT_EQ(udp::socket_bind(second, ipv4::UNSPECIFIED_ADDR, 50002), ERR_IN_USE);
    EXPECT_EQ(udp::socket_bind(second, g_host, 50002), ERR_IN_USE);
    EXPECT_EQ(udp::socket_bind(first, ipv4::UNSPECIFIED_ADDR, 50003), ERR_INVALID);

    udp::socket_close(first);
    EXPECT_EQ(udp::socket_bind(second, g_host, 50002), OK);
    udp::socket_close(second);
}

TEST(udp_socket, bind_hands_out_distinct_ephemeral_ports) {
    udp::udp_socket* first = udp::socket_open();
    udp::udp_socket* second = udp::socket_open();
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    EXPECT_EQ(udp::socket_bind(first, ipv4::UNSPECIFIED_ADDR, 0), OK);
    EXPECT_EQ(udp::socket_bind(second, ipv4::UNSPECIFIED_ADDR, 0), OK);

    EXPECT_GE(first->local_port, udp::EPHEMERAL_PORT_MIN);
    EXPECT_GE(second->local_port, udp::EPHEMERAL_PORT_MIN);
    EXPECT_NE(first->local_port, second->local_port);

    udp::socket_close(first);
    udp::socket_close(second);
}
