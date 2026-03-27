#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/dhcp.h"
#include "net/ipv4.h"
#include "net/udp.h"
#include "net/ethernet.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/string.h"

TEST_SUITE(dhcp_test);

// ============================================================================
// Packet structure size
// ============================================================================

TEST(dhcp_test, dhcp_packet_size) {
    EXPECT_EQ(sizeof(net::dhcp_packet), static_cast<size_t>(240));
}

// ============================================================================
// Build DISCOVER
// ============================================================================

TEST(dhcp_test, build_discover_basic) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0xFF, sizeof(buf));

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint32_t xid = net::htonl(0xDEADBEEF);

    size_t len = net::dhcp_build_discover(buf, sizeof(buf), mac, xid);
    ASSERT_TRUE(len > sizeof(net::dhcp_packet));

    auto* pkt = reinterpret_cast<const net::dhcp_packet*>(buf);

    // Verify fixed header fields
    EXPECT_EQ(pkt->op, net::DHCP_OP_BOOTREQUEST);
    EXPECT_EQ(pkt->htype, net::DHCP_HTYPE_ETHERNET);
    EXPECT_EQ(pkt->hlen, net::DHCP_HLEN_ETHERNET);
    EXPECT_EQ(pkt->hops, static_cast<uint8_t>(0));

    // Transaction ID
    EXPECT_EQ(pkt->xid, xid);

    // Flags: broadcast
    EXPECT_EQ(net::ntohs(pkt->flags), net::DHCP_FLAG_BROADCAST);

    // All IP fields should be zero
    EXPECT_EQ(pkt->ciaddr, static_cast<uint32_t>(0));
    EXPECT_EQ(pkt->yiaddr, static_cast<uint32_t>(0));
    EXPECT_EQ(pkt->siaddr, static_cast<uint32_t>(0));
    EXPECT_EQ(pkt->giaddr, static_cast<uint32_t>(0));

    // MAC in chaddr
    EXPECT_EQ(pkt->chaddr[0], static_cast<uint8_t>(0x52));
    EXPECT_EQ(pkt->chaddr[1], static_cast<uint8_t>(0x54));
    EXPECT_EQ(pkt->chaddr[2], static_cast<uint8_t>(0x00));
    EXPECT_EQ(pkt->chaddr[3], static_cast<uint8_t>(0x12));
    EXPECT_EQ(pkt->chaddr[4], static_cast<uint8_t>(0x34));
    EXPECT_EQ(pkt->chaddr[5], static_cast<uint8_t>(0x56));
    // Remaining chaddr bytes should be zero
    EXPECT_EQ(pkt->chaddr[6], static_cast<uint8_t>(0));
    EXPECT_EQ(pkt->chaddr[15], static_cast<uint8_t>(0));

    // DHCP magic cookie
    EXPECT_EQ(net::ntohl(pkt->magic), net::DHCP_MAGIC_COOKIE);
}

TEST(dhcp_test, build_discover_has_msg_type_option) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint32_t xid = net::htonl(0x12345678);

    size_t len = net::dhcp_build_discover(buf, sizeof(buf), mac, xid);
    ASSERT_TRUE(len > sizeof(net::dhcp_packet));

    // Scan options for Message Type = DISCOVER
    const uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t opts_len = len - sizeof(net::dhcp_packet);
    bool found_msg_type = false;

    size_t pos = 0;
    while (pos < opts_len) {
        uint8_t code = opts[pos];
        if (code == net::DHCP_OPT_END) break;
        if (code == net::DHCP_OPT_PAD) { pos++; continue; }
        if (pos + 1 >= opts_len) break;
        uint8_t opt_len = opts[pos + 1];
        if (pos + 2 + opt_len > opts_len) break;

        if (code == net::DHCP_OPT_MSG_TYPE && opt_len == 1) {
            EXPECT_EQ(opts[pos + 2], net::DHCP_MSG_DISCOVER);
            found_msg_type = true;
        }
        pos += 2 + opt_len;
    }

    EXPECT_TRUE(found_msg_type);
}

TEST(dhcp_test, build_discover_has_param_list) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint32_t xid = net::htonl(0x00000001);

    size_t len = net::dhcp_build_discover(buf, sizeof(buf), mac, xid);
    ASSERT_TRUE(len > sizeof(net::dhcp_packet));

    const uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t opts_len = len - sizeof(net::dhcp_packet);
    bool found_param_list = false;

    size_t pos = 0;
    while (pos < opts_len) {
        uint8_t code = opts[pos];
        if (code == net::DHCP_OPT_END) break;
        if (code == net::DHCP_OPT_PAD) { pos++; continue; }
        if (pos + 1 >= opts_len) break;
        uint8_t opt_len = opts[pos + 1];
        if (pos + 2 + opt_len > opts_len) break;

        if (code == net::DHCP_OPT_PARAM_LIST) {
            // Should request at least subnet mask, router, DNS
            bool has_mask = false, has_router = false, has_dns = false;
            for (uint8_t i = 0; i < opt_len; i++) {
                if (opts[pos + 2 + i] == net::DHCP_OPT_SUBNET_MASK) has_mask = true;
                if (opts[pos + 2 + i] == net::DHCP_OPT_ROUTER) has_router = true;
                if (opts[pos + 2 + i] == net::DHCP_OPT_DNS) has_dns = true;
            }
            EXPECT_TRUE(has_mask);
            EXPECT_TRUE(has_router);
            EXPECT_TRUE(has_dns);
            found_param_list = true;
        }
        pos += 2 + opt_len;
    }

    EXPECT_TRUE(found_param_list);
}

TEST(dhcp_test, build_discover_has_end_marker) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint32_t xid = net::htonl(0xCAFEBABE);

    size_t len = net::dhcp_build_discover(buf, sizeof(buf), mac, xid);
    ASSERT_TRUE(len > sizeof(net::dhcp_packet));

    // The last meaningful byte in the options should be END (255)
    const uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t opts_len = len - sizeof(net::dhcp_packet);
    bool found_end = false;
    size_t pos = 0;
    while (pos < opts_len) {
        uint8_t code = opts[pos];
        if (code == net::DHCP_OPT_END) { found_end = true; break; }
        if (code == net::DHCP_OPT_PAD) { pos++; continue; }
        if (pos + 1 >= opts_len) break;
        uint8_t opt_len = opts[pos + 1];
        pos += 2 + opt_len;
    }
    EXPECT_TRUE(found_end);
}

TEST(dhcp_test, build_discover_buffer_too_small) {
    uint8_t buf[16]; // Way too small
    uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint32_t xid = net::htonl(1);

    size_t len = net::dhcp_build_discover(buf, sizeof(buf), mac, xid);
    EXPECT_EQ(len, static_cast<size_t>(0));
}

TEST(dhcp_test, build_discover_null_args) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint8_t mac[6] = {0};

    EXPECT_EQ(net::dhcp_build_discover(nullptr, sizeof(buf), mac, 0),
              static_cast<size_t>(0));
    EXPECT_EQ(net::dhcp_build_discover(buf, sizeof(buf), nullptr, 0),
              static_cast<size_t>(0));
}

// ============================================================================
// Build REQUEST
// ============================================================================

TEST(dhcp_test, build_request_basic) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint32_t xid = net::htonl(0xDEADC0DE);
    uint32_t offered_ip = net::ipv4_addr(10, 0, 2, 15);
    uint32_t server_id = net::ipv4_addr(10, 0, 2, 2);

    size_t len = net::dhcp_build_request(buf, sizeof(buf), mac, xid,
                                         offered_ip, server_id);
    ASSERT_TRUE(len > sizeof(net::dhcp_packet));

    auto* pkt = reinterpret_cast<const net::dhcp_packet*>(buf);
    EXPECT_EQ(pkt->op, net::DHCP_OP_BOOTREQUEST);
    EXPECT_EQ(pkt->xid, xid);
    EXPECT_EQ(net::ntohl(pkt->magic), net::DHCP_MAGIC_COOKIE);
}

TEST(dhcp_test, build_request_has_correct_options) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint32_t xid = net::htonl(0xAAAABBBB);
    uint32_t offered_ip = net::ipv4_addr(192, 168, 1, 100);
    uint32_t server_id = net::ipv4_addr(192, 168, 1, 1);

    size_t len = net::dhcp_build_request(buf, sizeof(buf), mac, xid,
                                         offered_ip, server_id);
    ASSERT_TRUE(len > sizeof(net::dhcp_packet));

    const uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t opts_len = len - sizeof(net::dhcp_packet);

    bool found_msg_type = false;
    bool found_requested_ip = false;
    bool found_server_id = false;

    size_t pos = 0;
    while (pos < opts_len) {
        uint8_t code = opts[pos];
        if (code == net::DHCP_OPT_END) break;
        if (code == net::DHCP_OPT_PAD) { pos++; continue; }
        if (pos + 1 >= opts_len) break;
        uint8_t opt_len = opts[pos + 1];
        if (pos + 2 + opt_len > opts_len) break;

        if (code == net::DHCP_OPT_MSG_TYPE && opt_len == 1) {
            EXPECT_EQ(opts[pos + 2], net::DHCP_MSG_REQUEST);
            found_msg_type = true;
        }

        if (code == net::DHCP_OPT_REQUESTED_IP && opt_len == 4) {
            uint32_t val;
            string::memcpy(&val, opts + pos + 2, 4);
            EXPECT_EQ(net::ntohl(val), offered_ip);
            found_requested_ip = true;
        }

        if (code == net::DHCP_OPT_SERVER_ID && opt_len == 4) {
            uint32_t val;
            string::memcpy(&val, opts + pos + 2, 4);
            EXPECT_EQ(net::ntohl(val), server_id);
            found_server_id = true;
        }

        pos += 2 + opt_len;
    }

    EXPECT_TRUE(found_msg_type);
    EXPECT_TRUE(found_requested_ip);
    EXPECT_TRUE(found_server_id);
}

// ============================================================================
// Parse OFFER
// ============================================================================

// Helper: build a hand-crafted DHCP OFFER packet for testing
static size_t build_test_offer(uint8_t* buf, size_t buf_size, uint32_t xid,
                               uint32_t yiaddr, uint32_t mask, uint32_t gw,
                               uint32_t dns, uint32_t server_id,
                               uint32_t lease_time) {
    if (buf_size < sizeof(net::dhcp_packet) + 64) return 0;

    string::memset(buf, 0, buf_size);
    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->htype = net::DHCP_HTYPE_ETHERNET;
    pkt->hlen = net::DHCP_HLEN_ETHERNET;
    pkt->xid = xid;
    pkt->yiaddr = net::htonl(yiaddr);
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t pos = 0;

    // Option 53: Message Type
    opts[pos++] = net::DHCP_OPT_MSG_TYPE;
    opts[pos++] = 1;
    opts[pos++] = net::DHCP_MSG_OFFER;

    // Option 1: Subnet Mask
    if (mask != 0) {
        opts[pos++] = net::DHCP_OPT_SUBNET_MASK;
        opts[pos++] = 4;
        uint32_t v = net::htonl(mask);
        string::memcpy(opts + pos, &v, 4);
        pos += 4;
    }

    // Option 3: Router
    if (gw != 0) {
        opts[pos++] = net::DHCP_OPT_ROUTER;
        opts[pos++] = 4;
        uint32_t v = net::htonl(gw);
        string::memcpy(opts + pos, &v, 4);
        pos += 4;
    }

    // Option 6: DNS
    if (dns != 0) {
        opts[pos++] = net::DHCP_OPT_DNS;
        opts[pos++] = 4;
        uint32_t v = net::htonl(dns);
        string::memcpy(opts + pos, &v, 4);
        pos += 4;
    }

    // Option 54: Server ID
    if (server_id != 0) {
        opts[pos++] = net::DHCP_OPT_SERVER_ID;
        opts[pos++] = 4;
        uint32_t v = net::htonl(server_id);
        string::memcpy(opts + pos, &v, 4);
        pos += 4;
    }

    // Option 51: Lease Time
    if (lease_time != 0) {
        opts[pos++] = net::DHCP_OPT_LEASE_TIME;
        opts[pos++] = 4;
        uint32_t v = net::htonl(lease_time);
        string::memcpy(opts + pos, &v, 4);
        pos += 4;
    }

    // Option 255: End
    opts[pos++] = net::DHCP_OPT_END;

    return sizeof(net::dhcp_packet) + pos;
}

TEST(dhcp_test, parse_offer_full) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint32_t xid = net::htonl(0x11223344);
    uint32_t ip = net::ipv4_addr(10, 0, 2, 15);
    uint32_t mask = net::ipv4_addr(255, 255, 255, 0);
    uint32_t gw = net::ipv4_addr(10, 0, 2, 2);
    uint32_t dns = net::ipv4_addr(10, 0, 2, 3);
    uint32_t srv = net::ipv4_addr(10, 0, 2, 2);

    size_t len = build_test_offer(buf, sizeof(buf), xid, ip, mask, gw, dns, srv, 86400);
    ASSERT_TRUE(len > sizeof(net::dhcp_packet));

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(
        reinterpret_cast<const net::dhcp_packet*>(buf), len, &cfg);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(cfg.valid);
    EXPECT_EQ(cfg.msg_type, net::DHCP_MSG_OFFER);
    EXPECT_EQ(cfg.offered_ip, ip);
    EXPECT_EQ(cfg.subnet_mask, mask);
    EXPECT_EQ(cfg.gateway, gw);
    EXPECT_EQ(cfg.dns_server, dns);
    EXPECT_EQ(cfg.server_id, srv);
    EXPECT_EQ(cfg.lease_time, static_cast<uint32_t>(86400));
}

TEST(dhcp_test, parse_offer_minimal) {
    // OFFER with only message type and yiaddr, no other options
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->htype = net::DHCP_HTYPE_ETHERNET;
    pkt->hlen = net::DHCP_HLEN_ETHERNET;
    pkt->xid = net::htonl(0xAAAA);
    pkt->yiaddr = net::htonl(net::ipv4_addr(192, 168, 0, 50));
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    opts[0] = net::DHCP_OPT_MSG_TYPE;
    opts[1] = 1;
    opts[2] = net::DHCP_MSG_OFFER;
    opts[3] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + 4, &cfg);
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.msg_type, net::DHCP_MSG_OFFER);
    EXPECT_EQ(cfg.offered_ip, net::ipv4_addr(192, 168, 0, 50));
    EXPECT_EQ(cfg.subnet_mask, static_cast<uint32_t>(0));
    EXPECT_EQ(cfg.gateway, static_cast<uint32_t>(0));
    EXPECT_EQ(cfg.dns_server, static_cast<uint32_t>(0));
}

// ============================================================================
// Parse ACK
// ============================================================================

TEST(dhcp_test, parse_ack) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->htype = net::DHCP_HTYPE_ETHERNET;
    pkt->hlen = net::DHCP_HLEN_ETHERNET;
    pkt->xid = net::htonl(0xBBBB);
    pkt->yiaddr = net::htonl(net::ipv4_addr(172, 16, 0, 10));
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    opts[0] = net::DHCP_OPT_MSG_TYPE;
    opts[1] = 1;
    opts[2] = net::DHCP_MSG_ACK;
    opts[3] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + 4, &cfg);
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.msg_type, net::DHCP_MSG_ACK);
    EXPECT_EQ(cfg.offered_ip, net::ipv4_addr(172, 16, 0, 10));
}

// ============================================================================
// Parse NAK
// ============================================================================

TEST(dhcp_test, parse_nak) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    opts[0] = net::DHCP_OPT_MSG_TYPE;
    opts[1] = 1;
    opts[2] = net::DHCP_MSG_NAK;
    opts[3] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + 4, &cfg);
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.msg_type, net::DHCP_MSG_NAK);
}

// ============================================================================
// Parse edge cases
// ============================================================================

TEST(dhcp_test, parse_unknown_options_skipped) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->yiaddr = net::htonl(net::ipv4_addr(10, 10, 10, 10));
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t pos = 0;

    // Unknown option 200, length 3
    opts[pos++] = 200;
    opts[pos++] = 3;
    opts[pos++] = 0xAA;
    opts[pos++] = 0xBB;
    opts[pos++] = 0xCC;

    // Message type (should still be found after unknown option)
    opts[pos++] = net::DHCP_OPT_MSG_TYPE;
    opts[pos++] = 1;
    opts[pos++] = net::DHCP_MSG_OFFER;

    // Another unknown option
    opts[pos++] = 250;
    opts[pos++] = 1;
    opts[pos++] = 0xFF;

    opts[pos++] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + pos, &cfg);
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.msg_type, net::DHCP_MSG_OFFER);
    EXPECT_EQ(cfg.offered_ip, net::ipv4_addr(10, 10, 10, 10));
}

TEST(dhcp_test, parse_pad_options_skipped) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t pos = 0;

    // Several PAD bytes before the message type
    opts[pos++] = net::DHCP_OPT_PAD;
    opts[pos++] = net::DHCP_OPT_PAD;
    opts[pos++] = net::DHCP_OPT_PAD;

    opts[pos++] = net::DHCP_OPT_MSG_TYPE;
    opts[pos++] = 1;
    opts[pos++] = net::DHCP_MSG_ACK;

    opts[pos++] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + pos, &cfg);
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.msg_type, net::DHCP_MSG_ACK);
}

TEST(dhcp_test, parse_truncated_packet) {
    // Packet shorter than the fixed header
    uint8_t buf[100];
    string::memset(buf, 0, sizeof(buf));

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(
        reinterpret_cast<const net::dhcp_packet*>(buf), 100, &cfg);
    // Should fail: packet is smaller than dhcp_packet (240 bytes)
    EXPECT_FALSE(ok);
}

TEST(dhcp_test, parse_no_msg_type) {
    // Valid packet but no message type option
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    // Subnet mask but no message type
    opts[0] = net::DHCP_OPT_SUBNET_MASK;
    opts[1] = 4;
    uint32_t mask = net::htonl(0xFFFFFF00);
    string::memcpy(opts + 2, &mask, 4);
    opts[6] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + 7, &cfg);
    EXPECT_FALSE(ok);
}

TEST(dhcp_test, parse_wrong_op_code) {
    // BOOTREQUEST instead of BOOTREPLY
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREQUEST;  // wrong!
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    opts[0] = net::DHCP_OPT_MSG_TYPE;
    opts[1] = 1;
    opts[2] = net::DHCP_MSG_OFFER;
    opts[3] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + 4, &cfg);
    EXPECT_FALSE(ok);
}

TEST(dhcp_test, parse_wrong_magic) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->magic = net::htonl(0x12345678);  // wrong magic

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    opts[0] = net::DHCP_OPT_MSG_TYPE;
    opts[1] = 1;
    opts[2] = net::DHCP_MSG_OFFER;
    opts[3] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + 4, &cfg);
    EXPECT_FALSE(ok);
}

TEST(dhcp_test, parse_null_args) {
    net::dhcp_config cfg = {};
    EXPECT_FALSE(net::dhcp_parse_response(nullptr, 300, &cfg));

    uint8_t buf[net::DHCP_PACKET_MAX];
    EXPECT_FALSE(net::dhcp_parse_response(
        reinterpret_cast<const net::dhcp_packet*>(buf), 300, nullptr));
}

TEST(dhcp_test, parse_options_truncated_mid_option) {
    // Option header says length=10 but only 2 bytes of data remain
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    opts[0] = net::DHCP_OPT_MSG_TYPE;
    opts[1] = 1;
    opts[2] = net::DHCP_MSG_OFFER;

    // Truncated option: code=1, length=10 but packet ends after 2 more bytes
    opts[3] = net::DHCP_OPT_SUBNET_MASK;
    opts[4] = 10; // claims 10 bytes of data

    // Only provide sizeof(dhcp_packet) + 5 bytes of options total
    // The subnet mask option data would need bytes 5-14, but we end at 5
    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + 5, &cfg);
    // Should still succeed with just the message type parsed before the truncation
    ASSERT_TRUE(ok);
    EXPECT_EQ(cfg.msg_type, net::DHCP_MSG_OFFER);
    EXPECT_EQ(cfg.subnet_mask, static_cast<uint32_t>(0)); // truncated, not parsed
}

// ============================================================================
// Build DISCOVER → Parse roundtrip
// ============================================================================

TEST(dhcp_test, discover_roundtrip_mac_preserved) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    uint32_t xid = net::htonl(0x55667788);

    size_t len = net::dhcp_build_discover(buf, sizeof(buf), mac, xid);
    ASSERT_TRUE(len > 0);

    auto* pkt = reinterpret_cast<const net::dhcp_packet*>(buf);
    EXPECT_EQ(string::memcmp(pkt->chaddr, mac, 6), 0);
    EXPECT_EQ(pkt->xid, xid);
}

// ============================================================================
// Multiple DNS/Router entries (only first used)
// ============================================================================

TEST(dhcp_test, parse_multiple_dns_uses_first) {
    uint8_t buf[net::DHCP_PACKET_MAX];
    string::memset(buf, 0, sizeof(buf));

    auto* pkt = reinterpret_cast<net::dhcp_packet*>(buf);
    pkt->op = net::DHCP_OP_BOOTREPLY;
    pkt->magic = net::htonl(net::DHCP_MAGIC_COOKIE);
    pkt->yiaddr = net::htonl(net::ipv4_addr(10, 0, 2, 15));

    uint8_t* opts = buf + sizeof(net::dhcp_packet);
    size_t pos = 0;

    opts[pos++] = net::DHCP_OPT_MSG_TYPE;
    opts[pos++] = 1;
    opts[pos++] = net::DHCP_MSG_ACK;

    // DNS with two entries (8 bytes)
    opts[pos++] = net::DHCP_OPT_DNS;
    opts[pos++] = 8;
    uint32_t dns1 = net::htonl(net::ipv4_addr(8, 8, 8, 8));
    uint32_t dns2 = net::htonl(net::ipv4_addr(8, 8, 4, 4));
    string::memcpy(opts + pos, &dns1, 4);
    pos += 4;
    string::memcpy(opts + pos, &dns2, 4);
    pos += 4;

    opts[pos++] = net::DHCP_OPT_END;

    net::dhcp_config cfg = {};
    bool ok = net::dhcp_parse_response(pkt, sizeof(net::dhcp_packet) + pos, &cfg);
    ASSERT_TRUE(ok);
    // Should use the first DNS server
    EXPECT_EQ(cfg.dns_server, net::ipv4_addr(8, 8, 8, 8));
}
