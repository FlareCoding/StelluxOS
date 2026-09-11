#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "stub_interface.h"
#include "net/interface.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "net/eth.h"
#include "net/net.h"

TEST_SUITE(interface);

using namespace net;

static const ipv4::ipv4_config g_lan = {{{10, 0, 2, 15}}, {{255, 255, 255, 0}}, {{10, 0, 2, 2}}};

static size_t neighbors_learned_on(const interface* iface) {
    arp::arp_snapshot_entry entries[arp::TABLE_SIZE];
    size_t count = arp::snapshot(entries, arp::TABLE_SIZE);

    size_t owned = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].iface == iface) {
            owned++;
        }
    }
    return owned;
}

// --- lookup_by_name_finds_registered_interfaces_only ---
// Proves: the registry answers to the exact registered name and nothing else.

TEST(interface, lookup_by_name_finds_registered_interfaces_only) {
    interface* lo = find_loopback_interface();
    ASSERT_NOT_NULL(lo);

    EXPECT_TRUE(find_interface_by_name(lo->name()) == lo);
    EXPECT_NULL(find_interface_by_name("nosuch0"));
    EXPECT_NULL(find_interface_by_name(""));
    EXPECT_NULL(find_interface_by_name(nullptr));
}

// --- configure_gives_the_interface_the_identity ---
// Proves: a well-formed configuration is published whole and can be taken back.

TEST(interface, configure_gives_the_interface_the_identity) {
    stub_interface link(false);
    EXPECT_FALSE(link.ipv4_conf().configured());

    EXPECT_EQ(link.configure_ipv4(g_lan), OK);
    ipv4::ipv4_config conf = link.ipv4_conf();
    EXPECT_TRUE(conf.address == g_lan.address);
    EXPECT_TRUE(conf.netmask == g_lan.netmask);
    EXPECT_TRUE(conf.gateway == g_lan.gateway);

    link.unconfigure_ipv4();
    EXPECT_FALSE(link.ipv4_conf().configured());
}

// --- configure_rejects_what_is_not_a_host_on_a_subnet ---
// Proves: malformed configurations are refused and leave the current one in place.

TEST(interface, configure_rejects_what_is_not_a_host_on_a_subnet) {
    stub_interface link(false);
    EXPECT_EQ(link.configure_ipv4(g_lan), OK);

    const ipv4::ipv4_config rejected[] = {
        {{{10, 0, 2, 0}}, {{255, 255, 255, 0}}, {{0, 0, 0, 0}}},       // network address
        {{{10, 0, 2, 255}}, {{255, 255, 255, 0}}, {{0, 0, 0, 0}}},     // broadcast address
        {{{10, 0, 2, 15}}, {{255, 0, 255, 0}}, {{0, 0, 0, 0}}},        // mask with a hole
        {{{10, 0, 2, 15}}, {{0, 0, 0, 0}}, {{0, 0, 0, 0}}},            // no network at all
        {{{10, 0, 2, 15}}, {{255, 255, 255, 0}}, {{10, 0, 3, 1}}},     // gateway off the subnet
        {{{10, 0, 2, 15}}, {{255, 255, 255, 0}}, {{10, 0, 2, 15}}},    // gateway is this host
        {{{10, 0, 2, 15}}, {{255, 255, 255, 0}}, {{10, 0, 2, 255}}},   // gateway is the broadcast
        {{{127, 0, 0, 1}}, {{255, 0, 0, 0}}, {{0, 0, 0, 0}}},          // loopback address on a link
        {{{224, 0, 0, 1}}, {{255, 255, 255, 0}}, {{0, 0, 0, 0}}},      // multicast address
    };

    for (const ipv4::ipv4_config& conf : rejected) {
        EXPECT_EQ(link.configure_ipv4(conf), ERR_INVALID);
        EXPECT_TRUE(link.ipv4_conf().address == g_lan.address);
    }

    stub_interface lo(true);
    EXPECT_EQ(lo.configure_ipv4(g_lan), ERR_INVALID);
    EXPECT_EQ(lo.configure_ipv4({{{127, 0, 0, 1}}, {{255, 0, 0, 0}}, {{0, 0, 0, 0}}}), OK);

    link.unconfigure_ipv4();
}

// --- reconfigure_forgets_the_neighbors ---
// Proves: neighbors resolved under one identity do not survive a change of identity.

TEST(interface, reconfigure_forgets_the_neighbors) {
    stub_interface link(false);
    EXPECT_EQ(link.configure_ipv4(g_lan), OK);

    eth::mac_addr mac = {};
    EXPECT_EQ(arp::resolve(&link, g_lan.gateway, &mac), ERR_PENDING);
    EXPECT_EQ(neighbors_learned_on(&link), 1u);

    EXPECT_EQ(link.configure_ipv4({{{10, 0, 3, 15}}, {{255, 255, 255, 0}}, {{10, 0, 3, 2}}}), OK);
    EXPECT_EQ(neighbors_learned_on(&link), 0u);

    EXPECT_EQ(arp::resolve(&link, ipv4::ipv4_addr{{10, 0, 3, 2}}, &mac), ERR_PENDING);
    EXPECT_EQ(neighbors_learned_on(&link), 1u);

    link.unconfigure_ipv4();
    EXPECT_EQ(neighbors_learned_on(&link), 0u);
}

// --- reconfigure_lets_accepted_packets_leave ---
// Proves: a packet waiting on a neighbor when the identity changes still goes
// out once the neighbor answers, and the neighbor is not kept afterwards.

TEST(interface, reconfigure_lets_accepted_packets_leave) {
    stub_interface link(false);
    EXPECT_EQ(link.configure_ipv4(g_lan), OK);

    packet* waiting = packet::alloc();
    ASSERT_NOT_NULL(waiting);
    ASSERT_TRUE(waiting->reserve(eth::HEADER_LEN));
    ASSERT_NOT_NULL(waiting->put(ipv4::HEADER_LEN));
    waiting->set_iface(&link);
    EXPECT_EQ(arp::resolve_and_send(waiting, g_lan.gateway), OK);
    EXPECT_EQ(link.frames_sent(), static_cast<size_t>(1));

    link.unconfigure_ipv4();
    EXPECT_EQ(neighbors_learned_on(&link), 1u);

    const eth::mac_addr gateway_mac = {{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02}};
    packet* reply = packet::alloc();
    ASSERT_NOT_NULL(reply);
    ASSERT_TRUE(reply->reserve(eth::HEADER_LEN));
    auto* hdr = reinterpret_cast<arp::arp_header*>(reply->put(arp::HEADER_LEN));
    ASSERT_NOT_NULL(hdr);
    hdr->hw_type = htons(arp::HW_TYPE_ETHERNET);
    hdr->proto_type = htons(arp::PROTO_TYPE_IPV4);
    hdr->hw_len = eth::MAC_ADDR_LEN;
    hdr->proto_len = ipv4::ADDR_LEN;
    hdr->opcode = htons(arp::OP_REPLY);
    hdr->sender_hw_addr = gateway_mac;
    hdr->sender_proto_addr = g_lan.gateway;
    hdr->target_hw_addr = link.mac();
    hdr->target_proto_addr = g_lan.address;
    reply->set_iface(&link);
    EXPECT_EQ(arp::input(reply), OK);

    EXPECT_EQ(link.frames_sent(), static_cast<size_t>(2));
    const auto* frame = reinterpret_cast<const eth::eth_header*>(link.last_frame());
    EXPECT_TRUE(frame->dest == gateway_mac);
    EXPECT_EQ(ntohs(frame->type), eth::TYPE_IPV4);
    EXPECT_EQ(neighbors_learned_on(&link), 0u);
}
