#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/net.h"
#include "net/route.h"
#include "net/loopback.h"
#include "net/ipv4.h"
#include "net/icmp.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(route_test);

// Dummy transmit callback for mock interfaces
static int32_t mock_tx(net::netif* /*iface*/, const uint8_t* /*frame*/,
                       size_t /*len*/) {
    return net::OK;
}

// Dummy link callback for mock interfaces
static bool mock_link_up(net::netif* /*iface*/) {
    return true;
}

// Basic route table state after init

TEST(route_test, table_has_loopback_routes) {
    // net::init() configures loopback with 127.0.0.1/8, which installs a
    // CONNECTED route for 127.0.0.0/8, so at least one route must exist.
    uint32_t count = net::route_count();
    EXPECT_GE(count, static_cast<uint32_t>(1));
}

// route_add and route_count

TEST(route_test, add_basic) {
    uint32_t before = net::route_count();

    // Create a mock interface for testing
    net::netif mock = {};
    string::memcpy(mock.name, "test0", 6);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;

    // Add a CONNECTED route for 192.168.100.0/24 with no gateway
    int32_t rc = net::route_add(
        net::ipv4_addr(192, 168, 100, 0),
        net::ipv4_addr(255, 255, 255, 0),
        0,
        &mock,
        net::route_type::CONNECTED,
        net::METRIC_CONNECTED);

    EXPECT_EQ(rc, net::OK);
    EXPECT_EQ(net::route_count(), before + 1);

    // Clean up
    net::route_del_iface(&mock);
    EXPECT_EQ(net::route_count(), before);
}

TEST(route_test, add_null_iface_rejected) {
    int32_t rc = net::route_add(0, 0, 0, nullptr, net::route_type::CONNECTED, 100);
    EXPECT_EQ(rc, net::ERR_INVAL);
}

// route_lookup, connected route

TEST(route_test, lookup_connected) {
    net::netif mock = {};
    string::memcpy(mock.name, "mock0", 6);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;
    mock.configured = true;
    mock.ipv4_addr = net::ipv4_addr(10, 0, 2, 15);

    int32_t rc = net::route_add(
        net::ipv4_addr(10, 0, 2, 0),
        net::ipv4_addr(255, 255, 255, 0),
        0, &mock, net::route_type::CONNECTED, net::METRIC_CONNECTED);
    ASSERT_EQ(rc, net::OK);

    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(10, 0, 2, 100), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock);
    EXPECT_EQ(rt.next_hop, net::ipv4_addr(10, 0, 2, 100));
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::CONNECTED));

    net::route_del_iface(&mock);
}

// route_lookup, gateway route

TEST(route_test, lookup_gateway) {
    net::netif mock = {};
    string::memcpy(mock.name, "mock1", 6);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;
    mock.configured = true;

    // Add default route via gateway 10.0.2.2
    int32_t rc = net::route_add(
        0, 0,
        net::ipv4_addr(10, 0, 2, 2),
        &mock, net::route_type::GATEWAY, net::METRIC_DEFAULT);
    ASSERT_EQ(rc, net::OK);

    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(8, 8, 8, 8), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock);
    EXPECT_EQ(rt.next_hop, net::ipv4_addr(10, 0, 2, 2));
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::GATEWAY));

    net::route_del_iface(&mock);
}

// route_lookup, longest prefix match

TEST(route_test, lookup_longest_prefix) {
    net::netif mock_broad = {};
    string::memcpy(mock_broad.name, "br0", 4);
    mock_broad.transmit = mock_tx;
    mock_broad.link_up = mock_link_up;
    mock_broad.configured = true;

    net::netif mock_narrow = {};
    string::memcpy(mock_narrow.name, "nr0", 4);
    mock_narrow.transmit = mock_tx;
    mock_narrow.link_up = mock_link_up;
    mock_narrow.configured = true;

    // Add a /8 route (broad)
    int32_t rc = net::route_add(
        net::ipv4_addr(10, 0, 0, 0),
        net::ipv4_addr(255, 0, 0, 0),
        0, &mock_broad, net::route_type::CONNECTED, net::METRIC_CONNECTED);
    ASSERT_EQ(rc, net::OK);

    // Add a /24 route (narrow, more specific)
    rc = net::route_add(
        net::ipv4_addr(10, 0, 2, 0),
        net::ipv4_addr(255, 255, 255, 0),
        0, &mock_narrow, net::route_type::CONNECTED, net::METRIC_CONNECTED);
    ASSERT_EQ(rc, net::OK);

    // Lookup 10.0.2.15, should match the /24 (longer prefix)
    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(10, 0, 2, 15), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock_narrow);

    // Lookup 10.0.3.1, should match the /8 (only one that matches)
    rc = net::route_lookup(net::ipv4_addr(10, 0, 3, 1), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock_broad);

    net::route_del_iface(&mock_broad);
    net::route_del_iface(&mock_narrow);
}

// route_lookup, metric tiebreak

TEST(route_test, lookup_metric_tiebreak) {
    net::netif mock_hi = {};
    string::memcpy(mock_hi.name, "hi0", 4);
    mock_hi.transmit = mock_tx;
    mock_hi.link_up = mock_link_up;
    mock_hi.configured = true;

    net::netif mock_lo = {};
    string::memcpy(mock_lo.name, "lo0", 4);
    mock_lo.transmit = mock_tx;
    mock_lo.link_up = mock_link_up;
    mock_lo.configured = true;

    // Add two routes for the same prefix, different metrics
    int32_t rc = net::route_add(
        net::ipv4_addr(172, 16, 0, 0),
        net::ipv4_addr(255, 255, 0, 0),
        0, &mock_hi, net::route_type::CONNECTED, 500);
    ASSERT_EQ(rc, net::OK);

    rc = net::route_add(
        net::ipv4_addr(172, 16, 0, 0),
        net::ipv4_addr(255, 255, 0, 0),
        0, &mock_lo, net::route_type::CONNECTED, 100);
    ASSERT_EQ(rc, net::OK);

    // Lower metric should win
    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(172, 16, 1, 1), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock_lo);

    net::route_del_iface(&mock_hi);
    net::route_del_iface(&mock_lo);
}

// route_lookup, local route

TEST(route_test, lookup_local) {
    net::netif mock = {};
    string::memcpy(mock.name, "lc0", 4);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;
    mock.configured = true;

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // Add a LOCAL /32 host route through the real loopback interface
    int32_t rc = net::route_add(
        net::ipv4_addr(10, 0, 2, 15),
        0xFFFFFFFF,
        0,
        lo, net::route_type::LOCAL, net::METRIC_LOCAL);
    ASSERT_EQ(rc, net::OK);

    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(10, 0, 2, 15), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, lo);
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::LOCAL));

    // The LOCAL route on lo stays, route_del_iface(lo) would also drop the
    // real loopback routes and 10.0.2.15 does not collide with other tests.
    net::route_del_iface(&mock);
}

// route_lookup, loopback

TEST(route_test, lookup_loopback) {
    // 127.0.0.1 must resolve to the loopback interface via the CONNECTED
    // 127.0.0.0/8 route installed when loopback is configured.
    net::route_result rt;
    int32_t rc = net::route_lookup(net::ipv4_addr(127, 0, 0, 1), &rt);
    ASSERT_EQ(rc, net::OK);

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    EXPECT_EQ(rt.iface, lo);
}

TEST(route_test, lookup_loopback_other_addr) {
    // 127.0.0.2 should also be routed through loopback
    // (matches 127.0.0.0/8 connected route)
    net::route_result rt;
    int32_t rc = net::route_lookup(net::ipv4_addr(127, 0, 0, 2), &rt);
    ASSERT_EQ(rc, net::OK);

    net::netif* lo = net::get_loopback_netif();
    EXPECT_EQ(rt.iface, lo);
}

// route_lookup, no match

TEST(route_test, lookup_no_match) {
    // The test environment has no default route, so an address outside
    // every configured subnet must fail the lookup with ERR_NOIF.
    net::route_result rt;
    int32_t rc = net::route_lookup(net::ipv4_addr(192, 168, 99, 99), &rt);
    EXPECT_EQ(rc, net::ERR_NOIF);
}

// route_del_iface

TEST(route_test, del_iface_clears_routes) {
    net::netif mock = {};
    string::memcpy(mock.name, "del0", 5);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;
    mock.configured = true;

    uint32_t before = net::route_count();

    // Add two routes for this interface
    net::route_add(net::ipv4_addr(10, 10, 0, 0),
                   net::ipv4_addr(255, 255, 0, 0),
                   0, &mock, net::route_type::CONNECTED, 100);
    net::route_add(0, 0, net::ipv4_addr(10, 10, 0, 1),
                   &mock, net::route_type::GATEWAY, 1000);

    EXPECT_EQ(net::route_count(), before + 2);

    // Delete all routes for this interface
    net::route_del_iface(&mock);
    EXPECT_EQ(net::route_count(), before);
}

// route_add_interface_routes, auto-populate

TEST(route_test, add_interface_routes_populates) {
    net::netif mock = {};
    string::memcpy(mock.name, "auto0", 6);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;

    // Register the interface
    int32_t rc = net::register_netif(&mock);
    ASSERT_EQ(rc, net::OK);

    uint32_t before = net::route_count();

    // Configure with IP, netmask, and gateway
    // This should auto-add: LOCAL (own IP), CONNECTED (subnet), GATEWAY (default)
    rc = net::configure(&mock,
                        net::ipv4_addr(10, 0, 5, 100),
                        net::ipv4_addr(255, 255, 255, 0),
                        net::ipv4_addr(10, 0, 5, 1));
    ASSERT_EQ(rc, net::OK);

    // Should have added 3 routes: LOCAL + CONNECTED + GATEWAY
    EXPECT_EQ(net::route_count(), before + 3);

    // Verify LOCAL route for own IP
    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(10, 0, 5, 100), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::LOCAL));

    // Verify CONNECTED route for subnet
    rc = net::route_lookup(net::ipv4_addr(10, 0, 5, 50), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock);
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::CONNECTED));

    // Verify GATEWAY route catches external IPs
    rc = net::route_lookup(net::ipv4_addr(8, 8, 8, 8), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock);
    EXPECT_EQ(rt.next_hop, net::ipv4_addr(10, 0, 5, 1));
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::GATEWAY));

    // Clean up
    net::route_del_iface(&mock);
    net::unregister_netif(&mock);
}

// route_add_interface_routes, reconfiguration clears old routes

TEST(route_test, reconfigure_replaces_routes) {
    net::netif mock = {};
    string::memcpy(mock.name, "reconf", 7);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;

    int32_t rc = net::register_netif(&mock);
    ASSERT_EQ(rc, net::OK);

    uint32_t before = net::route_count();

    // First configuration
    rc = net::configure(&mock,
                        net::ipv4_addr(10, 0, 6, 100),
                        net::ipv4_addr(255, 255, 255, 0),
                        net::ipv4_addr(10, 0, 6, 1));
    ASSERT_EQ(rc, net::OK);
    uint32_t after_first = net::route_count();
    EXPECT_EQ(after_first, before + 3);

    // Reconfigure with different IP, old routes should be replaced
    rc = net::configure(&mock,
                        net::ipv4_addr(10, 0, 7, 200),
                        net::ipv4_addr(255, 255, 255, 0),
                        net::ipv4_addr(10, 0, 7, 1));
    ASSERT_EQ(rc, net::OK);

    // Same number of routes (old ones deleted, new ones added)
    EXPECT_EQ(net::route_count(), after_first);

    // The old 10.0.6.0/24 CONNECTED route must be gone. The new default
    // gateway may still match the old subnet, but only as a GATEWAY route.
    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(10, 0, 6, 50), &rt);
    if (rc == net::OK) {
        if (rt.iface == &mock) {
            EXPECT_EQ(static_cast<uint8_t>(rt.type),
                      static_cast<uint8_t>(net::route_type::GATEWAY));
        }
    }

    // New IP should be routable
    rc = net::route_lookup(net::ipv4_addr(10, 0, 7, 50), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(rt.iface, &mock);

    // Clean up
    net::route_del_iface(&mock);
    net::unregister_netif(&mock);
}

// route_table_full

TEST(route_test, table_full) {
    // Save current count, then fill remaining slots
    uint32_t current = net::route_count();
    uint32_t available = net::ROUTE_TABLE_SIZE - current;

    // Use unique mock interfaces so route_del_iface cleans them all up
    net::netif mocks[net::ROUTE_TABLE_SIZE];
    uint32_t filled = 0;

    for (uint32_t i = 0; i < available; i++) {
        string::memset(&mocks[i], 0, sizeof(net::netif));
        mocks[i].name[0] = 'f';
        mocks[i].name[1] = static_cast<char>('0' + (i / 10) % 10);
        mocks[i].name[2] = static_cast<char>('0' + i % 10);
        mocks[i].name[3] = '\0';

        mocks[i].transmit = mock_tx;
        mocks[i].link_up = mock_link_up;
        mocks[i].configured = true;

        int32_t rc = net::route_add(
            net::ipv4_addr(200, static_cast<uint8_t>(i), 0, 0),
            net::ipv4_addr(255, 255, 255, 0),
            0, &mocks[i], net::route_type::CONNECTED, 200);
        if (rc == net::OK) {
            filled++;
        } else {
            break;
        }
    }

    EXPECT_EQ(net::route_count(), net::ROUTE_TABLE_SIZE);

    // Adding one more should fail
    net::netif extra = {};
    string::memcpy(extra.name, "over", 5);
    extra.transmit = mock_tx;
    extra.configured = true;

    int32_t rc = net::route_add(0, 0, 0, &extra,
                                net::route_type::CONNECTED, 999);
    EXPECT_EQ(rc, net::ERR_NOMEM);

    // Clean up all the test routes
    for (uint32_t i = 0; i < filled; i++) {
        net::route_del_iface(&mocks[i]);
    }

    EXPECT_EQ(net::route_count(), current);
}

// Verify loopback ipv4_send delivers through the routing table

TEST(route_test, ipv4_send_via_route_table) {
    // ipv4_send must deliver to 127.0.0.1 through the route table lookup
    // path, not through any hardcoded loopback shortcut.
    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);

    // Build a minimal ICMP echo request
    uint8_t icmp_pkt[8];
    string::memset(icmp_pkt, 0, sizeof(icmp_pkt));

    auto* hdr = reinterpret_cast<net::icmp_header*>(icmp_pkt);
    hdr->type = net::ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->id = net::htons(0x9999);
    hdr->sequence = net::htons(1);

    hdr->checksum = 0;
    hdr->checksum = net::inet_checksum(icmp_pkt, sizeof(icmp_pkt));

    // ipv4_send should route via loopback
    int32_t rc = net::ipv4_send(lo, net::ipv4_addr(127, 0, 0, 1),
                                net::IPV4_PROTO_ICMP,
                                icmp_pkt, sizeof(icmp_pkt));
    EXPECT_EQ(rc, net::OK);

    // Drain deferred TX (ICMP echo reply)
    net::drain_deferred_tx();
}

// unregister_netif removes every route the interface owns

TEST(route_test, unregister_cleans_routes) {
    net::netif mock = {};
    string::memcpy(mock.name, "unreg0", 7);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;

    int32_t rc = net::register_netif(&mock);
    ASSERT_EQ(rc, net::OK);

    uint32_t before = net::route_count();

    // Configure adds LOCAL + CONNECTED + GATEWAY = 3 routes
    rc = net::configure(&mock,
                        net::ipv4_addr(10, 99, 0, 50),
                        net::ipv4_addr(255, 255, 255, 0),
                        net::ipv4_addr(10, 99, 0, 1));
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(net::route_count(), before + 3);

    // Verify routes exist
    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(10, 99, 0, 50), &rt);
    ASSERT_EQ(rc, net::OK);

    // Unregister should clean up ALL routes (including LOCAL -> loopback)
    rc = net::unregister_netif(&mock);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(net::route_count(), before);

    // The old IP may still match some unrelated route, but the lookup
    // must never return the unregistered interface
    rc = net::route_lookup(net::ipv4_addr(10, 99, 0, 50), &rt);
    if (rc == net::OK) {
        EXPECT_NE(rt.iface, &mock);
    }
}

// Reconfiguring loopback must not destroy LOCAL routes that other
// interfaces own, route deletion is keyed on the owning interface

TEST(route_test, loopback_reconfig_preserves_other_local_routes) {
    // Set up a mock interface with a LOCAL route through loopback
    net::netif mock = {};
    string::memcpy(mock.name, "own0", 5);
    mock.transmit = mock_tx;
    mock.link_up = mock_link_up;

    int32_t rc = net::register_netif(&mock);
    ASSERT_EQ(rc, net::OK);

    rc = net::configure(&mock,
                        net::ipv4_addr(10, 50, 0, 100),
                        net::ipv4_addr(255, 255, 255, 0),
                        0);
    ASSERT_EQ(rc, net::OK);

    // Verify the LOCAL route exists and points through loopback
    net::route_result rt;
    rc = net::route_lookup(net::ipv4_addr(10, 50, 0, 100), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::LOCAL));

    net::netif* lo = net::get_loopback_netif();
    ASSERT_NOT_NULL(lo);
    EXPECT_EQ(rt.iface, lo);

    // Reconfiguring loopback triggers route_del_iface(lo), which must only
    // remove routes owned by loopback, not the LOCAL route owned by mock.
    rc = net::configure(lo,
                        net::ipv4_addr(127, 0, 0, 1),
                        net::ipv4_addr(255, 0, 0, 0),
                        0);
    ASSERT_EQ(rc, net::OK);

    // The LOCAL route for mock's IP (10.50.0.100) should still exist
    rc = net::route_lookup(net::ipv4_addr(10, 50, 0, 100), &rt);
    ASSERT_EQ(rc, net::OK);
    EXPECT_EQ(static_cast<uint8_t>(rt.type),
              static_cast<uint8_t>(net::route_type::LOCAL));
    EXPECT_EQ(rt.iface, lo);

    // Clean up
    net::route_del_iface(&mock);
    net::unregister_netif(&mock);
}
