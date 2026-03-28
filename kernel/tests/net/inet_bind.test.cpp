#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/net.h"
#include "net/loopback.h"
#include "net/route.h"
#include "net/ipv4.h"
#include "net/udp.h"
#include "net/inet_socket.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/ethernet.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "resource/resource.h"
#include "mm/heap.h"

TEST_SUITE(inet_bind_test);

namespace {

// Helper: build a kernel_sockaddr_in for binding
net::kernel_sockaddr_in make_sin(uint32_t ip_host, uint16_t port_host) {
    net::kernel_sockaddr_in sa{};
    sa.sin_family = net::AF_INET_VAL;
    sa.sin_port = net::htons(port_host);
    sa.sin_addr = net::htonl(ip_host);
    return sa;
}

// Helper: create a UDP socket and return (obj, sock) pair.
// Caller must clean up via close_udp_socket.
struct udp_pair {
    resource::resource_object* obj;
    net::inet_socket* sock;
};

udp_pair create_udp() {
    resource::resource_object* obj = nullptr;
    int32_t rc = net::create_inet_udp_socket(&obj);
    if (rc != resource::OK || !obj) {
        return {nullptr, nullptr};
    }
    return {obj, static_cast<net::inet_socket*>(obj->impl)};
}

// Helper: bind a UDP socket to (ip, port) via ops
int32_t do_bind(resource::resource_object* obj, uint32_t ip_host,
                uint16_t port_host) {
    auto sa = make_sin(ip_host, port_host);
    return obj->ops->bind(obj, &sa, sizeof(sa));
}

// Helper: clean up a UDP socket (mirrors inet_close path)
void close_udp_socket(resource::resource_object* obj) {
    if (obj && obj->ops && obj->ops->close) {
        obj->ops->close(obj);
    }
    if (obj) {
        heap::kfree_delete(obj);
    }
}

} // namespace

// ============================================================================
// Basic bind
// ============================================================================

TEST(inet_bind_test, udp_bind_specific_port) {
    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);
    ASSERT_NOT_NULL(sock);

    int32_t rc = do_bind(obj, 0, 7777);
    EXPECT_EQ(rc, resource::OK);
    EXPECT_EQ(sock->bound_port, static_cast<uint16_t>(7777));
    EXPECT_EQ(sock->bound_addr, static_cast<uint32_t>(0));

    close_udp_socket(obj);
}

TEST(inet_bind_test, udp_bind_ephemeral) {
    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);

    int32_t rc = do_bind(obj, 0, 0);
    EXPECT_EQ(rc, resource::OK);
    EXPECT_GE(sock->bound_port, net::UDP_PORT_EPHEMERAL_MIN);
    EXPECT_LE(sock->bound_port, net::UDP_PORT_EPHEMERAL_MAX);

    close_udp_socket(obj);
}

TEST(inet_bind_test, udp_bind_already_bound) {
    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);

    ASSERT_EQ(do_bind(obj, 0, 7778), resource::OK);
    EXPECT_EQ(do_bind(obj, 0, 7779), resource::ERR_INVAL);
    EXPECT_EQ(sock->bound_port, static_cast<uint16_t>(7778));

    close_udp_socket(obj);
}

// ============================================================================
// Conflict detection
// ============================================================================

TEST(inet_bind_test, udp_bind_port_conflict_same_addr) {
    auto [obj_a, sock_a] = create_udp();
    auto [obj_b, sock_b] = create_udp();
    ASSERT_NOT_NULL(obj_a);
    ASSERT_NOT_NULL(obj_b);

    ASSERT_EQ(do_bind(obj_a, 0, 8888), resource::OK);
    EXPECT_EQ(do_bind(obj_b, 0, 8888), resource::ERR_ADDRINUSE);

    close_udp_socket(obj_b);
    close_udp_socket(obj_a);
}

TEST(inet_bind_test, udp_bind_port_conflict_wildcard_vs_specific) {
    auto [obj_a, sock_a] = create_udp();
    auto [obj_b, sock_b] = create_udp();
    ASSERT_NOT_NULL(obj_a);
    ASSERT_NOT_NULL(obj_b);

    // Wildcard bind blocks specific-address bind on same port
    ASSERT_EQ(do_bind(obj_a, 0, 5555), resource::OK);
    EXPECT_EQ(do_bind(obj_b, net::ipv4_addr(127, 0, 0, 1), 5555),
              resource::ERR_ADDRINUSE);

    close_udp_socket(obj_b);
    close_udp_socket(obj_a);
}

TEST(inet_bind_test, udp_bind_port_conflict_specific_vs_wildcard) {
    auto [obj_a, sock_a] = create_udp();
    auto [obj_b, sock_b] = create_udp();
    ASSERT_NOT_NULL(obj_a);
    ASSERT_NOT_NULL(obj_b);

    // Specific-address bind blocks wildcard bind on same port
    ASSERT_EQ(do_bind(obj_a, net::ipv4_addr(127, 0, 0, 1), 5556), resource::OK);
    EXPECT_EQ(do_bind(obj_b, 0, 5556), resource::ERR_ADDRINUSE);

    close_udp_socket(obj_b);
    close_udp_socket(obj_a);
}

TEST(inet_bind_test, udp_bind_same_port_different_addr_allowed) {
    // Two specific different local IPs on the same port should be allowed.
    // We need a mock NIC so there are two local IPs.
    static net::netif mock_eth = {};
    string::memcpy(mock_eth.name, "bnd0", 5);
    mock_eth.transmit = [](net::netif*, const uint8_t*, size_t) -> int32_t {
        return net::OK;
    };
    mock_eth.link_up = [](net::netif*) -> bool { return true; };
    mock_eth.poll = nullptr;
    mock_eth.driver_data = nullptr;

    int32_t rc = net::register_netif(&mock_eth);
    ASSERT_EQ(rc, net::OK);
    rc = net::configure(&mock_eth,
                        net::ipv4_addr(10, 0, 88, 100),
                        net::ipv4_addr(255, 255, 255, 0),
                        0);
    ASSERT_EQ(rc, net::OK);

    auto [obj_a, sock_a] = create_udp();
    auto [obj_b, sock_b] = create_udp();
    ASSERT_NOT_NULL(obj_a);
    ASSERT_NOT_NULL(obj_b);

    ASSERT_EQ(do_bind(obj_a, net::ipv4_addr(127, 0, 0, 1), 6666), resource::OK);
    EXPECT_EQ(do_bind(obj_b, net::ipv4_addr(10, 0, 88, 100), 6666), resource::OK);

    close_udp_socket(obj_b);
    close_udp_socket(obj_a);

    net::route_del_iface(&mock_eth);
    net::unregister_netif(&mock_eth);
}

// ============================================================================
// Implicit bind interaction
// ============================================================================

TEST(inet_bind_test, udp_sendto_then_bind_fails) {
    // After sendto assigns an ephemeral port, explicit bind must fail.
    // We need a default netif for sendto to work.
    static net::netif mock_eth = {};
    string::memcpy(mock_eth.name, "bnd1", 5);
    mock_eth.transmit = [](net::netif*, const uint8_t*, size_t) -> int32_t {
        return net::OK;
    };
    mock_eth.link_up = [](net::netif*) -> bool { return true; };
    mock_eth.poll = nullptr;
    mock_eth.driver_data = nullptr;

    int32_t rc = net::register_netif(&mock_eth);
    ASSERT_EQ(rc, net::OK);
    rc = net::configure(&mock_eth,
                        net::ipv4_addr(10, 0, 99, 50),
                        net::ipv4_addr(255, 255, 255, 0),
                        0);
    ASSERT_EQ(rc, net::OK);

    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);
    ASSERT_NOT_NULL(obj->ops);
    ASSERT_TRUE(obj->ops->sendto != nullptr);

    // sendto triggers implicit ephemeral bind
    auto dst = make_sin(net::ipv4_addr(127, 0, 0, 1), 9999);
    uint8_t payload[] = "test";
    (void)obj->ops->sendto(obj, payload, 4, 0, &dst, sizeof(dst));

    // sendto may fail if routing/loopback isn't quite right, but the
    // ephemeral port assignment happens before the send attempt.
    // What matters is that bound_port is now non-zero.
    EXPECT_NE(sock->bound_port, static_cast<uint16_t>(0));

    // Explicit bind must fail
    EXPECT_EQ(do_bind(obj, 0, 12345), resource::ERR_INVAL);

    close_udp_socket(obj);
    net::route_del_iface(&mock_eth);
    net::unregister_netif(&mock_eth);
}

// ============================================================================
// Address validation
// ============================================================================

TEST(inet_bind_test, udp_bind_loopback_addr) {
    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);

    int32_t rc = do_bind(obj, net::ipv4_addr(127, 0, 0, 1), 9999);
    EXPECT_EQ(rc, resource::OK);
    EXPECT_EQ(sock->bound_addr, net::ipv4_addr(127, 0, 0, 1));
    EXPECT_EQ(sock->bound_port, static_cast<uint16_t>(9999));

    close_udp_socket(obj);
}

TEST(inet_bind_test, udp_bind_nonlocal_addr_rejected) {
    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);

    int32_t rc = do_bind(obj, net::ipv4_addr(8, 8, 8, 8), 9999);
    EXPECT_EQ(rc, resource::ERR_INVAL);
    EXPECT_EQ(sock->bound_port, static_cast<uint16_t>(0));

    close_udp_socket(obj);
}

TEST(inet_bind_test, udp_bind_bad_family) {
    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);

    net::kernel_sockaddr_in sa{};
    sa.sin_family = 99; // not AF_INET
    sa.sin_port = net::htons(7777);
    sa.sin_addr = 0;
    int32_t rc = obj->ops->bind(obj, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::ERR_INVAL);
    EXPECT_EQ(sock->bound_port, static_cast<uint16_t>(0));

    close_udp_socket(obj);
}

// ============================================================================
// Address-filtered delivery via loopback
// ============================================================================

TEST(inet_bind_test, udp_recv_delivers_to_bound_addr) {
    // Bind a UDP socket to 127.0.0.1:7000, inject a UDP frame
    // addressed to 127.0.0.1:7000, verify it is delivered.
    auto [obj, sock] = create_udp();
    ASSERT_NOT_NULL(obj);
    ASSERT_NOT_NULL(sock);
    ASSERT_NOT_NULL(sock->rx_buf);

    ASSERT_EQ(do_bind(obj, net::ipv4_addr(127, 0, 0, 1), 7000), resource::OK);

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // Build UDP payload
    const uint8_t udp_payload[] = "hello-bind";
    constexpr size_t payload_len = 10;

    // Build UDP header + payload
    size_t udp_total = sizeof(net::udp_header) + payload_len;
    uint8_t udp_pkt[64];
    string::memset(udp_pkt, 0, sizeof(udp_pkt));
    auto* uhdr = reinterpret_cast<net::udp_header*>(udp_pkt);
    uhdr->src_port = net::htons(12345);
    uhdr->dst_port = net::htons(7000);
    uhdr->length = net::htons(static_cast<uint16_t>(udp_total));
    uhdr->checksum = 0;
    string::memcpy(udp_pkt + sizeof(net::udp_header), udp_payload, payload_len);

    // Compute UDP checksum
    uint16_t csum = net::udp_checksum(
        net::htonl(net::ipv4_addr(127, 0, 0, 1)),
        net::htonl(net::ipv4_addr(127, 0, 0, 1)),
        udp_pkt, udp_total);
    uhdr->checksum = (csum == 0) ? static_cast<uint16_t>(0xFFFF) : csum;

    // Build IPv4 header
    size_t ip_total = sizeof(net::ipv4_header) + udp_total;
    uint8_t ip_pkt[128];
    string::memset(ip_pkt, 0, sizeof(ip_pkt));
    auto* ip_hdr = reinterpret_cast<net::ipv4_header*>(ip_pkt);
    ip_hdr->ver_ihl = (4 << 4) | 5;
    ip_hdr->total_len = net::htons(static_cast<uint16_t>(ip_total));
    ip_hdr->ttl = 64;
    ip_hdr->protocol = net::IPV4_PROTO_UDP;
    ip_hdr->src_ip = net::htonl(net::ipv4_addr(127, 0, 0, 1));
    ip_hdr->dst_ip = net::htonl(net::ipv4_addr(127, 0, 0, 1));
    ip_hdr->checksum = 0;
    string::memcpy(ip_pkt + sizeof(net::ipv4_header), udp_pkt, udp_total);
    ip_hdr->checksum = net::inet_checksum(ip_pkt, sizeof(net::ipv4_header));

    // Build Ethernet frame
    size_t frame_len = sizeof(net::eth_header) + ip_total;
    uint8_t frame[256];
    string::memset(frame, 0, sizeof(frame));
    auto* eth_hdr = reinterpret_cast<net::eth_header*>(frame);
    string::memset(eth_hdr->dst, 0, net::MAC_ADDR_LEN);
    string::memset(eth_hdr->src, 0, net::MAC_ADDR_LEN);
    eth_hdr->ethertype = net::htons(net::ETH_TYPE_IPV4);
    string::memcpy(frame + sizeof(net::eth_header), ip_pkt, ip_total);

    // Transmit through loopback
    int32_t tx_rc = lo->transmit(lo, frame, frame_len);
    EXPECT_EQ(tx_rc, net::OK);

    // Read from the socket's ring buffer (nonblock)
    // UDP RX framing: [4 src_ip_net][2 src_port_net][2 payload_len][N payload]
    uint8_t read_buf[128];
    ssize_t nread = ring_buffer_read(sock->rx_buf, read_buf, sizeof(read_buf), true);
    EXPECT_GT(nread, static_cast<ssize_t>(0));

    if (nread >= 8) {
        uint16_t recv_payload_len;
        string::memcpy(&recv_payload_len, read_buf + 6, 2);
        EXPECT_EQ(recv_payload_len, static_cast<uint16_t>(payload_len));

        if (recv_payload_len == payload_len && nread >= 8 + static_cast<ssize_t>(payload_len)) {
            EXPECT_EQ(string::memcmp(read_buf + 8, udp_payload, payload_len), 0);
        }
    }

    close_udp_socket(obj);
}
