#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/net.h"
#include "net/loopback.h"
#include "net/netinfo.h"
#include "net/ipv4.h"
#include "net/icmp.h"
#include "net/arp.h"
#include "net/ethernet.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/inet_socket.h"
#include "common/string.h"
#include "common/ring_buffer.h"
#include "resource/resource.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(loopback_test);

// Shared initialization flag — used by both loopback_test and route_test
// since tests run before boot code reaches net::init().
bool g_net_initialized = false;

namespace {

int32_t loopback_before_all() {
    if (!g_net_initialized) {
        int32_t rc = net::init();
        if (rc != net::OK) return -1;
        g_net_initialized = true;
    }
    return 0;
}

int32_t loopback_after_all() {
    return 0;
}

} // namespace

BEFORE_ALL(loopback_test, loopback_before_all);
AFTER_ALL(loopback_test, loopback_after_all);

// ============================================================================
// Interface existence and identity
// ============================================================================

TEST(loopback_test, interface_exists) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
}

TEST(loopback_test, name_is_lo) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    EXPECT_STREQ(lo->name, "lo");
}

TEST(loopback_test, ip_configured) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    EXPECT_EQ(lo->ipv4_addr, net::ipv4_addr(127, 0, 0, 1));
    EXPECT_EQ(lo->ipv4_netmask, net::ipv4_addr(255, 0, 0, 0));
    EXPECT_EQ(lo->ipv4_gateway, static_cast<uint32_t>(0));
}

TEST(loopback_test, is_configured_flag) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    EXPECT_TRUE(lo->configured);
}

TEST(loopback_test, link_always_up) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    ASSERT_TRUE(lo->link_up != nullptr);
    EXPECT_TRUE(lo->link_up(lo));
}

TEST(loopback_test, mac_is_zero) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    for (size_t i = 0; i < net::MAC_ADDR_LEN; i++) {
        EXPECT_EQ(lo->mac[i], static_cast<uint8_t>(0));
    }
}

TEST(loopback_test, has_transmit_callback) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    ASSERT_TRUE(lo->transmit != nullptr);
}

TEST(loopback_test, poll_is_null) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    EXPECT_TRUE(lo->poll == nullptr);
}

// ============================================================================
// Default interface behavior
// ============================================================================

TEST(loopback_test, not_default_interface) {
    // In the test environment with no hardware NICs, the default
    // outbound interface should be nullptr — NOT loopback.
    net::netif* def = net::get_default_netif();
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // If a default exists, it must not be loopback
    if (def != nullptr) {
        EXPECT_NE(def, lo);
    }
    // In a test-only environment, there might be no default at all,
    // which is correct behavior.
}

// ============================================================================
// Interface list visibility
// ============================================================================

TEST(loopback_test, visible_in_interface_list) {
    net::net_status status = {};
    int32_t rc = net::query_status(&status);
    ASSERT_EQ(rc, net::OK);
    ASSERT_TRUE(status.if_count > 0);

    bool found_lo = false;
    for (uint32_t i = 0; i < status.if_count; i++) {
        if (string::strcmp(status.interfaces[i].name, "lo") == 0) {
            found_lo = true;
            // Verify the reported config matches
            EXPECT_EQ(status.interfaces[i].ipv4_addr,
                      net::ipv4_addr(127, 0, 0, 1));
            EXPECT_EQ(status.interfaces[i].ipv4_netmask,
                      net::ipv4_addr(255, 0, 0, 0));
            EXPECT_BITS_SET(status.interfaces[i].flags, net::IFF_CONFIGURED);
            EXPECT_BITS_SET(status.interfaces[i].flags, net::IFF_UP);
            break;
        }
    }
    EXPECT_TRUE(found_lo);
}

// ============================================================================
// Loopback transmit — frame delivery
// ============================================================================

TEST(loopback_test, transmit_delivers_to_rx) {
    // Create an ICMP socket to receive packets delivered through loopback
    resource::resource_object* obj = nullptr;
    int32_t rc = net::create_inet_icmp_socket(&obj);
    ASSERT_EQ(rc, resource::OK);
    ASSERT_NOT_NULL(obj);
    ASSERT_NOT_NULL(obj->impl);

    auto* sock = static_cast<net::inet_socket*>(obj->impl);
    ASSERT_NOT_NULL(sock->rx_buf);

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // Build an ICMP echo request
    uint8_t icmp_pkt[64];
    string::memset(icmp_pkt, 0, sizeof(icmp_pkt));
    auto* icmp_hdr = reinterpret_cast<net::icmp_header*>(icmp_pkt);
    icmp_hdr->type = net::ICMP_TYPE_ECHO_REQUEST;
    icmp_hdr->code = 0;
    icmp_hdr->id = net::htons(0x1234);
    icmp_hdr->sequence = net::htons(1);
    icmp_hdr->checksum = 0;
    // Fill payload with a pattern
    for (size_t i = sizeof(net::icmp_header); i < sizeof(icmp_pkt); i++) {
        icmp_pkt[i] = static_cast<uint8_t>(i & 0xFF);
    }
    icmp_hdr->checksum = net::inet_checksum(icmp_pkt, sizeof(icmp_pkt));

    // Build IPv4 header
    size_t ip_total = sizeof(net::ipv4_header) + sizeof(icmp_pkt);
    uint8_t ip_pkt[128];
    string::memset(ip_pkt, 0, sizeof(ip_pkt));
    auto* ip_hdr = reinterpret_cast<net::ipv4_header*>(ip_pkt);
    ip_hdr->ver_ihl = (4 << 4) | 5;
    ip_hdr->total_len = net::htons(static_cast<uint16_t>(ip_total));
    ip_hdr->ttl = 64;
    ip_hdr->protocol = net::IPV4_PROTO_ICMP;
    ip_hdr->src_ip = net::htonl(net::ipv4_addr(127, 0, 0, 1));
    ip_hdr->dst_ip = net::htonl(net::ipv4_addr(127, 0, 0, 1));
    ip_hdr->checksum = 0;
    string::memcpy(ip_pkt + sizeof(net::ipv4_header), icmp_pkt, sizeof(icmp_pkt));
    ip_hdr->checksum = net::inet_checksum(ip_pkt, sizeof(net::ipv4_header));

    // Build Ethernet header
    size_t frame_len = sizeof(net::eth_header) + ip_total;
    uint8_t frame[256];
    string::memset(frame, 0, sizeof(frame));
    auto* eth_hdr = reinterpret_cast<net::eth_header*>(frame);
    string::memset(eth_hdr->dst, 0, net::MAC_ADDR_LEN);
    string::memset(eth_hdr->src, 0, net::MAC_ADDR_LEN);
    eth_hdr->ethertype = net::htons(net::ETH_TYPE_IPV4);
    string::memcpy(frame + sizeof(net::eth_header), ip_pkt, ip_total);

    // Transmit through loopback — this should deliver to ICMP handler
    rc = lo->transmit(lo, frame, frame_len);
    EXPECT_EQ(rc, net::OK);

    // The ICMP handler should have queued an echo reply via deferred TX
    // and delivered the original echo request to our socket.
    // Drain deferred TX to process the echo reply.
    net::drain_deferred_tx();

    // Try to read from the socket's ring buffer (nonblock)
    // The ICMP delivery format: [4 bytes src_ip_net] [2 bytes payload_len] [N bytes data]
    uint8_t read_buf[256];
    ssize_t nread = ring_buffer_read(sock->rx_buf, read_buf, sizeof(read_buf), true);

    // We should have received at least the echo request delivery
    EXPECT_GT(nread, static_cast<ssize_t>(0));

    // Clean up — unregister socket and destroy resources
    net::icmp_unregister_socket(sock);
    if (sock->rx_buf) {
        ring_buffer_destroy(sock->rx_buf);
        sock->rx_buf = nullptr;
    }
    heap::kfree_delete(sock);
    heap::kfree_delete(obj);
}

// ============================================================================
// ipv4_send to loopback destination
// ============================================================================

TEST(loopback_test, ipv4_send_to_127_0_0_1) {
    // Create an ICMP socket to catch delivered packets
    resource::resource_object* obj = nullptr;
    int32_t rc = net::create_inet_icmp_socket(&obj);
    ASSERT_EQ(rc, resource::OK);
    ASSERT_NOT_NULL(obj);

    auto* sock = static_cast<net::inet_socket*>(obj->impl);
    ASSERT_NOT_NULL(sock);
    ASSERT_NOT_NULL(sock->rx_buf);

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // Build an ICMP echo request
    uint8_t icmp_pkt[64];
    string::memset(icmp_pkt, 0, sizeof(icmp_pkt));
    auto* icmp_hdr = reinterpret_cast<net::icmp_header*>(icmp_pkt);
    icmp_hdr->type = net::ICMP_TYPE_ECHO_REQUEST;
    icmp_hdr->code = 0;
    icmp_hdr->id = net::htons(0x5678);
    icmp_hdr->sequence = net::htons(2);
    icmp_hdr->checksum = 0;
    icmp_hdr->checksum = net::inet_checksum(icmp_pkt, sizeof(icmp_pkt));

    // Send via ipv4_send to 127.0.0.1
    // This should: build IP header → loopback check → eth_send → lo_transmit
    // → rx_frame → eth_recv → ipv4_recv → icmp_recv → deliver_to_sockets
    rc = net::ipv4_send(lo, net::ipv4_addr(127, 0, 0, 1),
                        net::IPV4_PROTO_ICMP, icmp_pkt, sizeof(icmp_pkt));
    EXPECT_EQ(rc, net::OK);

    // Drain deferred TX — the echo reply was queued by icmp_recv
    net::drain_deferred_tx();

    // Read from socket — should have received the echo request
    // ICMP delivery format: [4 bytes src_ip_net][2 bytes payload_len][N bytes data]
    uint8_t read_buf[256];
    ssize_t nread = ring_buffer_read(sock->rx_buf, read_buf, sizeof(read_buf), true);
    EXPECT_GT(nread, static_cast<ssize_t>(0));

    if (nread >= 6) {
        // Parse the framing header
        uint32_t src_ip_net;
        string::memcpy(&src_ip_net, read_buf, 4);
        uint32_t src_ip = net::ntohl(src_ip_net);
        EXPECT_EQ(src_ip, net::ipv4_addr(127, 0, 0, 1));

        uint16_t payload_len;
        string::memcpy(&payload_len, read_buf + 4, 2);
        EXPECT_GE(payload_len, static_cast<uint16_t>(sizeof(net::icmp_header)));

        // Check the ICMP type in the delivered payload
        if (payload_len >= sizeof(net::icmp_header)) {
            auto* delivered = reinterpret_cast<const net::icmp_header*>(read_buf + 6);
            EXPECT_EQ(delivered->type, net::ICMP_TYPE_ECHO_REQUEST);
            EXPECT_EQ(delivered->id, net::htons(0x5678));
            EXPECT_EQ(delivered->sequence, net::htons(2));
        }
    }

    // There should also be a second packet: the echo reply from drain_deferred_tx
    ssize_t nread2 = ring_buffer_read(sock->rx_buf, read_buf, sizeof(read_buf), true);
    if (nread2 >= 6) {
        uint16_t payload_len2;
        string::memcpy(&payload_len2, read_buf + 4, 2);
        if (payload_len2 >= sizeof(net::icmp_header)) {
            auto* reply = reinterpret_cast<const net::icmp_header*>(read_buf + 6);
            EXPECT_EQ(reply->type, net::ICMP_TYPE_ECHO_REPLY);
        }
    }

    // Clean up
    net::icmp_unregister_socket(sock);
    if (sock->rx_buf) {
        ring_buffer_destroy(sock->rx_buf);
        sock->rx_buf = nullptr;
    }
    heap::kfree_delete(sock);
    heap::kfree_delete(obj);
}

// ============================================================================
// ipv4_send to other 127.x.x.x addresses
// ============================================================================

TEST(loopback_test, ipv4_send_to_127_0_0_2) {
    // Verify that sending to 127.0.0.2 also routes through loopback.
    // The packet should be accepted by ipv4_recv since the loopback
    // interface accepts any address in 127.0.0.0/8.
    resource::resource_object* obj = nullptr;
    int32_t rc = net::create_inet_icmp_socket(&obj);
    ASSERT_EQ(rc, resource::OK);
    ASSERT_NOT_NULL(obj);

    auto* sock = static_cast<net::inet_socket*>(obj->impl);
    ASSERT_NOT_NULL(sock);

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // Build a minimal ICMP echo request
    uint8_t icmp_pkt[8];
    string::memset(icmp_pkt, 0, sizeof(icmp_pkt));
    auto* icmp_hdr = reinterpret_cast<net::icmp_header*>(icmp_pkt);
    icmp_hdr->type = net::ICMP_TYPE_ECHO_REQUEST;
    icmp_hdr->code = 0;
    icmp_hdr->id = net::htons(0xABCD);
    icmp_hdr->sequence = net::htons(1);
    icmp_hdr->checksum = 0;
    icmp_hdr->checksum = net::inet_checksum(icmp_pkt, sizeof(icmp_pkt));

    // Send to 127.0.0.2
    rc = net::ipv4_send(lo, net::ipv4_addr(127, 0, 0, 2),
                        net::IPV4_PROTO_ICMP, icmp_pkt, sizeof(icmp_pkt));
    EXPECT_EQ(rc, net::OK);

    net::drain_deferred_tx();

    // Should have received the packet
    uint8_t read_buf[128];
    ssize_t nread = ring_buffer_read(sock->rx_buf, read_buf, sizeof(read_buf), true);
    EXPECT_GT(nread, static_cast<ssize_t>(0));

    // Clean up
    net::icmp_unregister_socket(sock);
    if (sock->rx_buf) {
        ring_buffer_destroy(sock->rx_buf);
        sock->rx_buf = nullptr;
    }
    heap::kfree_delete(sock);
    heap::kfree_delete(obj);
}

// ============================================================================
// Loopback transmit with null/invalid args
// ============================================================================

TEST(loopback_test, transmit_null_frame) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    ASSERT_TRUE(lo->transmit != nullptr);

    int32_t rc = lo->transmit(lo, nullptr, 100);
    EXPECT_EQ(rc, net::ERR_INVAL);
}

TEST(loopback_test, transmit_zero_length) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    ASSERT_TRUE(lo->transmit != nullptr);

    uint8_t dummy[1] = {0};
    int32_t rc = lo->transmit(lo, dummy, 0);
    EXPECT_EQ(rc, net::ERR_INVAL);
}

TEST(loopback_test, transmit_null_iface) {
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    ASSERT_TRUE(lo->transmit != nullptr);

    uint8_t dummy[64] = {};
    int32_t rc = lo->transmit(nullptr, dummy, sizeof(dummy));
    EXPECT_EQ(rc, net::ERR_INVAL);
}
