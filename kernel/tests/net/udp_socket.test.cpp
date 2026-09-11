#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "stub_interface.h"
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

// --- deliver_respects_the_bound_interface ---
// Proves: a socket bound to an interface receives from it alone, an unbound
// socket on the same port receives from anywhere.

TEST(udp_socket, deliver_respects_the_bound_interface) {
    stub_interface mine;
    stub_interface other;

    udp::udp_socket* sock = udp::socket_open();
    ASSERT_NOT_NULL(sock);
    ASSERT_EQ(udp::socket_bind(sock, ipv4::UNSPECIFIED_ADDR, 50004), OK);
    sock->iface = &mine;

    packet* elsewhere = make_datagram(50004);
    ASSERT_NOT_NULL(elsewhere);
    elsewhere->set_iface(&other);
    EXPECT_EQ(udp::socket_deliver(elsewhere), ERR_NOT_FOUND);
    packet::free(elsewhere);

    packet* here = make_datagram(50004);
    ASSERT_NOT_NULL(here);
    here->set_iface(&mine);
    EXPECT_EQ(udp::socket_deliver(here), OK);
    EXPECT_EQ(sock->rx_queue.size(), static_cast<size_t>(1));

    udp::socket_close(sock);
}

// --- pinned_sockets_share_a_port_across_interfaces ---
// Proves: a port can be bound once per interface, a socket on every interface
// or on the same one still conflicts, each datagram reaches its own link's
// socket, and a later move cannot break what bind enforced.

TEST(udp_socket, pinned_sockets_share_a_port_across_interfaces) {
    stub_interface link_a;
    stub_interface link_b;

    udp::udp_socket* on_a = udp::socket_open();
    udp::udp_socket* on_b = udp::socket_open();
    udp::udp_socket* anywhere = udp::socket_open();
    udp::udp_socket* also_on_a = udp::socket_open();
    ASSERT_NOT_NULL(on_a);
    ASSERT_NOT_NULL(on_b);
    ASSERT_NOT_NULL(anywhere);
    ASSERT_NOT_NULL(also_on_a);
    on_a->iface = &link_a;
    on_b->iface = &link_b;
    also_on_a->iface = &link_a;

    EXPECT_EQ(udp::socket_bind(on_a, ipv4::UNSPECIFIED_ADDR, 50006), OK);
    EXPECT_EQ(udp::socket_bind(on_b, ipv4::UNSPECIFIED_ADDR, 50006), OK);
    EXPECT_EQ(udp::socket_bind(anywhere, ipv4::UNSPECIFIED_ADDR, 50006), ERR_IN_USE);
    EXPECT_EQ(udp::socket_bind(also_on_a, ipv4::UNSPECIFIED_ADDR, 50006), ERR_IN_USE);

    packet* for_b = make_datagram(50006);
    ASSERT_NOT_NULL(for_b);
    for_b->set_iface(&link_b);
    EXPECT_EQ(udp::socket_deliver(for_b), OK);
    EXPECT_TRUE(on_a->rx_queue.empty());
    EXPECT_EQ(on_b->rx_queue.size(), static_cast<size_t>(1));

    // Moving a bound socket obeys the same rule as binding it there
    stub_interface link_c;
    EXPECT_EQ(udp::socket_bind_to_device(on_b, &link_a), ERR_IN_USE);
    EXPECT_EQ(udp::socket_bind_to_device(on_b, nullptr), ERR_IN_USE);
    EXPECT_EQ(on_b->iface, &link_b);
    EXPECT_EQ(udp::socket_bind_to_device(on_b, &link_c), OK);
    EXPECT_EQ(on_b->iface, &link_c);

    udp::socket_close(on_a);
    udp::socket_close(on_b);
    udp::socket_close(anywhere);
    udp::socket_close(also_on_a);
}

// --- output_through_an_unconfigured_interface_broadcasts_from_nowhere ---
// Proves: a datagram pinned to an interface without an address leaves it as a
// link broadcast with the unspecified source, the way an address is asked for.

TEST(udp_socket, output_through_an_unconfigured_interface_broadcasts_from_nowhere) {
    stub_interface link;

    packet* pkt = packet::alloc();
    ASSERT_NOT_NULL(pkt);
    ASSERT_TRUE(pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN + udp::HEADER_LEN));
    uint8_t* body = pkt->put(4);
    ASSERT_NOT_NULL(body);
    string::memcpy(body, "dhcp", 4);

    ASSERT_EQ(udp::output(pkt, &link, ipv4::BROADCAST_ADDR, 68, 67, true), OK);
    ASSERT_EQ(link.frames_sent(), static_cast<size_t>(1));

    const auto* frame = reinterpret_cast<const eth::eth_header*>(link.last_frame());
    EXPECT_TRUE(frame->dest == eth::BROADCAST_ADDR);

    const auto* ip = reinterpret_cast<const ipv4::ipv4_header*>(link.last_frame() + eth::HEADER_LEN);
    EXPECT_TRUE(ip->src.is_unspecified());
    EXPECT_TRUE(ip->dst.is_broadcast());

    const auto* hdr = reinterpret_cast<const udp::udp_header*>(link.last_frame() + eth::HEADER_LEN + ipv4::HEADER_LEN);
    EXPECT_EQ(ntohs(hdr->src_port), 68);
    EXPECT_EQ(ntohs(hdr->dst_port), 67);
}

// --- output_refuses_a_broadcast_the_socket_did_not_opt_into ---
// Proves: a broadcast destination needs SO_BROADCAST.

TEST(udp_socket, output_refuses_a_broadcast_the_socket_did_not_opt_into) {
    stub_interface link;
    ASSERT_EQ(link.configure_ipv4({{{10, 0, 2, 15}}, {{255, 255, 255, 0}}, {{0, 0, 0, 0}}}), OK);

    packet* pkt = packet::alloc();
    ASSERT_NOT_NULL(pkt);
    ASSERT_TRUE(pkt->reserve(eth::HEADER_LEN + ipv4::HEADER_LEN + udp::HEADER_LEN));
    ASSERT_NOT_NULL(pkt->put(1));

    EXPECT_EQ(udp::output(pkt, &link, ipv4::ipv4_addr{{10, 0, 2, 255}}, 50005, 7, false), ERR_ACCESS);
    EXPECT_EQ(link.frames_sent(), static_cast<size_t>(0));

    link.unconfigure_ipv4();
}
