#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/net.h"
#include "net/loopback.h"
#include "net/inet_socket.h"
#include "net/udp.h"
#include "net/ipv4.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "resource/resource.h"
#include "mm/heap.h"

TEST_SUITE(inet_socket_test);

extern bool g_net_initialized;

namespace {

int32_t inet_socket_before_all() {
    if (!g_net_initialized) {
        int32_t rc = net::init();
        if (rc != net::OK) return -1;
        g_net_initialized = true;
    }
    return 0;
}

// Helper: build a kernel_sockaddr_in in host structs
net::kernel_sockaddr_in make_sockaddr(uint16_t port, uint32_t addr_host = 0) {
    net::kernel_sockaddr_in sa = {};
    sa.sin_family = net::AF_INET_VAL;
    sa.sin_port = net::htons(port);
    sa.sin_addr = net::htonl(addr_host);
    return sa;
}

// Helper: clean up an inet socket resource_object. Handles ICMP
// and UDP unregistration, ring buffer teardown, and freeing.
void cleanup_inet_obj(resource::resource_object* obj) {
    if (!obj) return;
    resource::resource_release(obj);
}

} // namespace

BEFORE_ALL(inet_socket_test, inet_socket_before_all);

// ============================================================================
// UDP bind — basic functionality
// ============================================================================

TEST(inet_socket_test, udp_bind_specific_port) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);
    ASSERT_NOT_NULL(obj);
    ASSERT_NOT_NULL(obj->ops);
    ASSERT_TRUE(obj->ops->bind != nullptr);

    auto* sock = static_cast<net::inet_socket*>(obj->impl);
    ASSERT_NOT_NULL(sock);

    auto sa = make_sockaddr(5000);
    int32_t rc = obj->ops->bind(obj, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::OK);
    EXPECT_EQ(sock->state, net::INET_STATE_BOUND);
    EXPECT_EQ(sock->bound_port, static_cast<uint16_t>(5000));
    EXPECT_EQ(sock->bound_addr, static_cast<uint32_t>(0));

    cleanup_inet_obj(obj);
}

TEST(inet_socket_test, udp_bind_port_zero_ephemeral) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);
    auto* sock = static_cast<net::inet_socket*>(obj->impl);

    auto sa = make_sockaddr(0);
    int32_t rc = obj->ops->bind(obj, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::OK);
    EXPECT_EQ(sock->state, net::INET_STATE_BOUND);
    EXPECT_GT(sock->bound_port, static_cast<uint16_t>(0));

    cleanup_inet_obj(obj);
}

TEST(inet_socket_test, udp_bind_conflict) {
    resource::resource_object* obj_a = nullptr;
    resource::resource_object* obj_b = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj_a), resource::OK);
    ASSERT_EQ(net::create_inet_udp_socket(&obj_b), resource::OK);

    auto sa = make_sockaddr(6000);
    int32_t rc = obj_a->ops->bind(obj_a, &sa, sizeof(sa));
    ASSERT_EQ(rc, resource::OK);

    rc = obj_b->ops->bind(obj_b, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::ERR_ADDRINUSE);

    cleanup_inet_obj(obj_b);
    cleanup_inet_obj(obj_a);
}

TEST(inet_socket_test, udp_double_bind_rejected) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);

    auto sa = make_sockaddr(7000);
    ASSERT_EQ(obj->ops->bind(obj, &sa, sizeof(sa)), resource::OK);

    // Second bind on the same socket should fail
    auto sa2 = make_sockaddr(7001);
    int32_t rc = obj->ops->bind(obj, &sa2, sizeof(sa2));
    EXPECT_EQ(rc, resource::ERR_INVAL);

    cleanup_inet_obj(obj);
}

TEST(inet_socket_test, udp_bind_after_sendto_rejected) {
    // sendto auto-assigns an ephemeral port; subsequent bind should fail
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);

    auto* sock = static_cast<net::inet_socket*>(obj->impl);

    // Simulate what sendto does: assign ephemeral + register
    sock->bound_port = net::udp_alloc_ephemeral_port();
    net::udp_register_socket(sock);

    auto sa = make_sockaddr(7500);
    int32_t rc = obj->ops->bind(obj, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::ERR_INVAL);

    cleanup_inet_obj(obj);
}

TEST(inet_socket_test, udp_close_frees_port) {
    // Bind to a port, close, re-bind to same port should succeed
    resource::resource_object* obj1 = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj1), resource::OK);

    auto sa = make_sockaddr(8000);
    ASSERT_EQ(obj1->ops->bind(obj1, &sa, sizeof(sa)), resource::OK);
    cleanup_inet_obj(obj1);

    // Port 8000 should now be free
    resource::resource_object* obj2 = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj2), resource::OK);
    int32_t rc = obj2->ops->bind(obj2, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::OK);

    cleanup_inet_obj(obj2);
}

TEST(inet_socket_test, udp_bind_wrong_family) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);

    // Construct a sockaddr with AF_UNIX family
    net::kernel_sockaddr_in sa = {};
    sa.sin_family = 1;  // AF_UNIX
    sa.sin_port = net::htons(8100);

    int32_t rc = obj->ops->bind(obj, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::ERR_INVAL);

    cleanup_inet_obj(obj);
}

TEST(inet_socket_test, udp_bind_nonlocal_addr) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);

    // 8.8.8.8 is not a local address
    auto sa = make_sockaddr(8200, net::ipv4_addr(8, 8, 8, 8));
    int32_t rc = obj->ops->bind(obj, &sa, sizeof(sa));
    EXPECT_EQ(rc, resource::ERR_INVAL);

    cleanup_inet_obj(obj);
}

// ============================================================================
// UDP bind — data path over loopback
// ============================================================================

TEST(inet_socket_test, udp_bound_socket_receives) {
    // Bind a UDP socket to port 9000, send a UDP packet to
    // 127.0.0.1:9000 via ipv4_send, verify delivery.
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);

    auto sa = make_sockaddr(9000);
    ASSERT_EQ(obj->ops->bind(obj, &sa, sizeof(sa)), resource::OK);

    auto* sock = static_cast<net::inet_socket*>(obj->impl);
    ASSERT_NOT_NULL(sock->rx_buf);

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // Build a UDP packet: header + payload
    const char* payload = "hello-bind";
    size_t payload_len = 10;
    size_t udp_total = sizeof(net::udp_header) + payload_len;

    uint8_t udp_pkt[64];
    string::memset(udp_pkt, 0, sizeof(udp_pkt));
    auto* uhdr = reinterpret_cast<net::udp_header*>(udp_pkt);
    uhdr->src_port = net::htons(12345);  // arbitrary source port
    uhdr->dst_port = net::htons(9000);
    uhdr->length = net::htons(static_cast<uint16_t>(udp_total));
    uhdr->checksum = 0;
    string::memcpy(udp_pkt + sizeof(net::udp_header), payload, payload_len);

    // Compute UDP checksum with pseudo-header
    uint32_t lo_ip = net::ipv4_addr(127, 0, 0, 1);
    uint16_t csum = net::udp_checksum(
        net::htonl(lo_ip), net::htonl(lo_ip), udp_pkt, udp_total);
    uhdr->checksum = (csum == 0) ? static_cast<uint16_t>(0xFFFF) : csum;

    // Send via ipv4_send → loopback → udp_recv → socket delivery
    int32_t rc = net::ipv4_send(lo, lo_ip, net::IPV4_PROTO_UDP,
                                udp_pkt, udp_total);
    ASSERT_EQ(rc, net::OK);

    // Drain deferred TX
    net::drain_deferred_tx();

    // Read from socket's ring buffer (nonblock)
    // UDP delivery format: [4 src_ip_net][2 src_port_net][2 payload_len][N data]
    uint8_t read_buf[128];
    ssize_t nread = ring_buffer_read(sock->rx_buf, read_buf, sizeof(read_buf), true);
    EXPECT_GT(nread, static_cast<ssize_t>(8));
    if (nread <= static_cast<ssize_t>(8)) {
        cleanup_inet_obj(obj);
        return;
    }

    // Verify payload content
    uint16_t recv_payload_len;
    string::memcpy(&recv_payload_len, read_buf + 6, 2);
    EXPECT_EQ(recv_payload_len, static_cast<uint16_t>(payload_len));

    if (nread >= static_cast<ssize_t>(8 + payload_len)) {
        EXPECT_EQ(string::memcmp(read_buf + 8, payload, payload_len), 0);
    }

    cleanup_inet_obj(obj);
}

// ============================================================================
// Ops dispatch validation
// ============================================================================

TEST(inet_socket_test, icmp_bind_not_supported) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_icmp_socket(&obj), resource::OK);
    ASSERT_NOT_NULL(obj->ops);

    // ICMP sockets should not have bind
    EXPECT_TRUE(obj->ops->bind == nullptr);

    cleanup_inet_obj(obj);
}

TEST(inet_socket_test, dgram_listen_not_supported) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);

    EXPECT_TRUE(obj->ops->listen == nullptr);

    cleanup_inet_obj(obj);
}

TEST(inet_socket_test, dgram_accept_not_supported) {
    resource::resource_object* obj = nullptr;
    ASSERT_EQ(net::create_inet_udp_socket(&obj), resource::OK);

    EXPECT_TRUE(obj->ops->accept == nullptr);

    cleanup_inet_obj(obj);
}
