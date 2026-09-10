#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "stub_interface.h"
#include "net/route.h"
#include "net/interface.h"
#include "net/ipv4.h"
#include "net/net.h"

TEST_SUITE(route);

using namespace net;

// --- limited_broadcast_never_uses_loopback ---
// Proves: the limited broadcast goes to a link or nowhere, loopback has no link
// to carry it even though it is registered first.

TEST(route, limited_broadcast_never_uses_loopback) {
    route::route_result out = {};
    int32_t rc = route::lookup(ipv4::BROADCAST_ADDR, &out);

    if (rc == OK) {
        EXPECT_FALSE(out.iface->is_loopback());
        EXPECT_FALSE(out.source.is_loopback());
    } else {
        EXPECT_EQ(rc, ERR_NO_ROUTE);
    }
}

// --- loopback_network_stays_local ---
// Proves: the loopback network itself is still this host, both its host
// addresses and its broadcast.

TEST(route, loopback_network_stays_local) {
    route::route_result out = {};
    ASSERT_EQ(route::lookup(ipv4::ipv4_addr{{127, 0, 0, 1}}, &out), OK);
    EXPECT_TRUE(out.iface->is_loopback());
    EXPECT_TRUE(out.type == route::route_type::local);

    ASSERT_EQ(route::lookup(ipv4::ipv4_addr{{127, 255, 255, 255}}, &out), OK);
    EXPECT_TRUE(out.iface->is_loopback());
    EXPECT_TRUE(out.type == route::route_type::broadcast);
}

// --- unconfigured_interface_can_only_broadcast ---
// Proves: a link without an address still reaches the limited broadcast, from
// the unspecified source, and nothing else.

TEST(route, unconfigured_interface_can_only_broadcast) {
    stub_interface link;
    route::route_result out = {};

    ASSERT_EQ(route::lookup_on(&link, ipv4::BROADCAST_ADDR, &out), OK);
    EXPECT_TRUE(out.iface == &link);
    EXPECT_TRUE(out.type == route::route_type::broadcast);
    EXPECT_TRUE(out.source.is_unspecified());

    EXPECT_EQ(route::lookup_on(&link, ipv4::ipv4_addr{{10, 0, 2, 2}}, &out), ERR_NO_ROUTE);
}
