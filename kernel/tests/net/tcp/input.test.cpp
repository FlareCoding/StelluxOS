#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "harness.h"

TEST_SUITE(tcp_input);

using namespace net;
using namespace net::tcp;

static void expect_reset_frame(const linked_peer& lp, size_t index) {
    const ipv4::ipv4_header* ip = sent_ip(lp.link, index);
    EXPECT_EQ(ip->proto, ipv4::PROTO_TCP);
    EXPECT_TRUE(ip->src == lp.remote.host);
    EXPECT_TRUE(ip->dst == lp.remote.addr);

    const tcp_header* hdr = sent_tcp(lp.link, index);
    EXPECT_EQ(ntohs(hdr->src_port), lp.remote.host_port);
    EXPECT_EQ(ntohs(hdr->dst_port), lp.remote.port);
    EXPECT_EQ(hdr->data_offset(), MIN_DATA_OFFSET);
    EXPECT_EQ(ntohs(hdr->window), 0);
    EXPECT_TRUE(is_checksum_valid(ip->src, ip->dst, hdr, HEADER_LEN));
    EXPECT_EQ(lp.link.frame_len(index), eth::HEADER_LEN + ipv4::HEADER_LEN + HEADER_LEN);
}

TEST(tcp_input, syn_to_a_closed_port_is_answered_with_a_reset_acknowledging_it) {
    linked_peer lp;

    EXPECT_EQ(input(lp.remote.syn(1000)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);

    expect_reset_frame(lp, 0);
    const tcp_header* hdr = sent_tcp(lp.link, 0);
    EXPECT_EQ(hdr->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->seq), 0u);
    EXPECT_EQ(ntohl(hdr->ack), 1001u);
}

TEST(tcp_input, segment_with_ack_is_answered_with_a_reset_at_its_acknowledgment) {
    linked_peer lp;

    EXPECT_EQ(input(lp.remote.ack(1000, 777)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);

    expect_reset_frame(lp, 0);
    const tcp_header* hdr = sent_tcp(lp.link, 0);
    EXPECT_EQ(hdr->flags, FLAG_RST);
    EXPECT_EQ(ntohl(hdr->seq), 777u);
    EXPECT_EQ(ntohl(hdr->ack), 0u);
}

TEST(tcp_input, reset_acknowledges_payload_and_fin_as_segment_length) {
    linked_peer lp;
    const char payload[] = "hello";

    EXPECT_EQ(input(lp.remote.segment(FLAG_FIN | FLAG_PSH, 5000, 0, {}, payload, 5)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);

    const tcp_header* hdr = sent_tcp(lp.link, 0);
    EXPECT_EQ(hdr->flags, FLAG_RST | FLAG_ACK);
    EXPECT_EQ(ntohl(hdr->ack), 5006u);
}

TEST(tcp_input, reset_acknowledgment_wraps_past_zero) {
    linked_peer lp;

    EXPECT_EQ(input(lp.remote.syn(0xFFFFFFFF)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);

    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->ack), 0u);
}

TEST(tcp_input, reset_is_never_answered) {
    linked_peer lp;

    EXPECT_EQ(input(lp.remote.rst(1000)), OK);
    EXPECT_EQ(input(lp.remote.segment(FLAG_RST | FLAG_ACK, 1000, 5)), OK);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_input, syn_with_options_is_still_answered) {
    linked_peer lp;
    tcp_options opts;
    opts.mss = 1460;
    opts.sack_permitted = true;
    opts.has_timestamps = true;
    opts.ts_val = 99;
    opts.window_scale = 7;

    EXPECT_EQ(input(lp.remote.segment(FLAG_SYN, 42, 0, opts)), OK);
    ASSERT_EQ(lp.link.frames_sent(), 1u);
    EXPECT_EQ(ntohl(sent_tcp(lp.link, 0)->ack), 43u);
}

TEST(tcp_input, bad_checksum_is_dropped_silently) {
    linked_peer lp;
    packet* pkt = lp.remote.syn(1000);
    reinterpret_cast<tcp_header*>(pkt->data())->checksum ^= 0x0001;

    EXPECT_EQ(input(pkt), ERR_INVALID);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_input, header_shorter_than_its_data_offset_is_dropped_silently) {
    linked_peer lp;
    packet* pkt = lp.remote.syn(1000);
    tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
    hdr->set_data_offset(MAX_DATA_OFFSET);
    hdr->checksum = 0;
    hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, HEADER_LEN));

    EXPECT_EQ(input(pkt), ERR_INVALID);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_input, malformed_option_is_dropped_silently) {
    linked_peer lp;
    tcp_options opts;
    opts.mss = 1460;
    packet* pkt = lp.remote.segment(FLAG_SYN, 1000, 0, opts);

    uint8_t* option_len = pkt->data() + HEADER_LEN + 1;
    *option_len = 0;
    tcp_header* hdr = reinterpret_cast<tcp_header*>(pkt->data());
    hdr->checksum = 0;
    hdr->checksum = htons(compute_checksum(lp.remote.addr, lp.remote.host, hdr, pkt->length()));

    EXPECT_EQ(input(pkt), ERR_INVALID);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_input, segment_to_a_broadcast_address_is_discarded) {
    linked_peer lp;

    peer to_subnet_broadcast = lp.remote;
    to_subnet_broadcast.host = {{10, 0, 2, 255}};
    EXPECT_EQ(input(to_subnet_broadcast.syn(1000)), OK);

    peer to_limited_broadcast = lp.remote;
    to_limited_broadcast.host = ipv4::BROADCAST_ADDR;
    EXPECT_EQ(input(to_limited_broadcast.syn(1000)), OK);

    EXPECT_EQ(lp.link.frames_sent(), 0u);
}

TEST(tcp_input, segment_in_a_broadcast_frame_is_discarded) {
    linked_peer lp;
    packet* pkt = lp.remote.syn(1000);
    reinterpret_cast<eth::eth_header*>(pkt->link_header())->dest = eth::BROADCAST_ADDR;

    EXPECT_EQ(input(pkt), OK);
    EXPECT_EQ(lp.link.frames_sent(), 0u);
}
