#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "net/ipv4.h"

TEST_SUITE(ipv4);

using net::ipv4::ipv4_addr;
using net::ipv4::ipv4_config;

static ipv4_addr addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ipv4_addr{{a, b, c, d}};
}

// A host on an ordinary /24 and the loopback interface it will one day have
static const ipv4_config g_eth_conf = {{{10, 0, 2, 15}}, {{255, 255, 255, 0}}, {{10, 0, 2, 2}}};
static const ipv4_config g_lo_conf = {{{127, 0, 0, 1}}, {{255, 0, 0, 0}}, {{0, 0, 0, 0}}};
static const ipv4_config g_unconfigured = {};

TEST(ipv4, unicast_accepts_single_hosts) {
    EXPECT_TRUE(g_eth_conf.is_unicast(addr(10, 0, 2, 2)));
    EXPECT_TRUE(g_eth_conf.is_unicast(addr(10, 0, 2, 15)));
    EXPECT_TRUE(g_eth_conf.is_unicast(addr(1, 0, 0, 1)));
    EXPECT_TRUE(g_eth_conf.is_unicast(addr(128, 0, 0, 1)));
    EXPECT_TRUE(g_eth_conf.is_unicast(addr(223, 255, 255, 255)));

    // Another subnet's broadcast is indistinguishable from a host without its mask
    EXPECT_TRUE(g_eth_conf.is_unicast(addr(10, 0, 3, 255)));
}

TEST(ipv4, unicast_rejects_rfc1122_non_hosts) {
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(0, 0, 0, 0)));
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(0, 0, 0, 5)));
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(10, 0, 2, 255)));
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(224, 0, 0, 0)));
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(239, 255, 255, 255)));
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(240, 0, 0, 0)));
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(255, 255, 255, 255)));
}

TEST(ipv4, unicast_loopback_only_on_loopback_interface) {
    EXPECT_FALSE(g_eth_conf.is_unicast(addr(127, 0, 0, 1)));
    EXPECT_FALSE(g_unconfigured.is_unicast(addr(127, 0, 0, 1)));

    EXPECT_TRUE(g_lo_conf.is_unicast(addr(127, 0, 0, 1)));
    EXPECT_TRUE(g_lo_conf.is_unicast(addr(127, 0, 0, 2)));
    EXPECT_TRUE(g_lo_conf.is_unicast(addr(10, 0, 2, 2)));
    EXPECT_FALSE(g_lo_conf.is_unicast(addr(127, 255, 255, 255)));
}

TEST(ipv4, unicast_without_configuration_knows_no_subnet) {
    EXPECT_TRUE(g_unconfigured.is_unicast(addr(10, 0, 2, 255)));
    EXPECT_TRUE(g_unconfigured.is_unicast(addr(10, 0, 2, 2)));
    EXPECT_FALSE(g_unconfigured.is_unicast(addr(255, 255, 255, 255)));
    EXPECT_FALSE(g_unconfigured.is_unicast(addr(0, 0, 0, 0)));
}
