#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
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
