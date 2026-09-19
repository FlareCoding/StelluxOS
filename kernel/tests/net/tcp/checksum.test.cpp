#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "net/tcp/wire.h"
#include "net/byteorder.h"
#include "common/string.h"

TEST_SUITE(tcp_checksum);

using namespace net;
using namespace net::tcp;

// Vectors computed independently of this code from the RFC 1071 procedure
constexpr uint16_t SYN_CHECKSUM              = 0xD780;
constexpr uint16_t SYN_TO_OTHER_HOST_CHECKSUM = 0xD77F;
constexpr uint16_t ACK_ABC_CHECKSUM          = 0x0D1C;

static const ipv4::ipv4_addr g_src   = {{10, 0, 2, 15}};
static const ipv4::ipv4_addr g_dst   = {{10, 0, 2, 2}};
static const ipv4::ipv4_addr g_other = {{10, 0, 2, 3}};

static tcp_header header(uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                         uint8_t flags, uint16_t window) {
    tcp_header hdr = {};
    hdr.src_port = htons(src_port);
    hdr.dst_port = htons(dst_port);
    hdr.seq = htonl(seq);
    hdr.ack = htonl(ack);
    hdr.set_data_offset(MIN_DATA_OFFSET);
    hdr.flags = flags;
    hdr.window = htons(window);

    return hdr;
}

static tcp_header syn_header() {
    return header(49152, 80, 1, 0, FLAG_SYN, 65535);
}

TEST(tcp_checksum, header_needs_twenty_bytes_and_a_data_offset_of_at_least_five) {
    tcp_header hdr = syn_header();

    EXPECT_TRUE(is_header_valid(&hdr, HEADER_LEN));
    EXPECT_TRUE(is_header_valid(&hdr, HEADER_LEN + 100));
    EXPECT_FALSE(is_header_valid(&hdr, HEADER_LEN - 1));

    hdr.set_data_offset(MIN_DATA_OFFSET - 1);
    EXPECT_FALSE(is_header_valid(&hdr, HEADER_LEN + 100));

    hdr.set_data_offset(0);
    EXPECT_FALSE(is_header_valid(&hdr, HEADER_LEN + 100));
}

TEST(tcp_checksum, header_with_options_must_fit_in_the_segment) {
    tcp_header hdr = syn_header();
    hdr.set_data_offset(MAX_DATA_OFFSET);

    EXPECT_TRUE(is_header_valid(&hdr, MAX_HEADER_LEN));
    EXPECT_TRUE(is_header_valid(&hdr, MAX_HEADER_LEN + 1));
    EXPECT_FALSE(is_header_valid(&hdr, MAX_HEADER_LEN - 1));
    EXPECT_FALSE(is_header_valid(&hdr, HEADER_LEN));

    hdr.set_data_offset(MIN_DATA_OFFSET + 1);
    EXPECT_TRUE(is_header_valid(&hdr, HEADER_LEN + 4));
    EXPECT_FALSE(is_header_valid(&hdr, HEADER_LEN + 3));
}

TEST(tcp_checksum, matches_the_reference_for_a_bare_syn) {
    tcp_header hdr = syn_header();

    EXPECT_EQ(compute_checksum(g_src, g_dst, &hdr, HEADER_LEN), SYN_CHECKSUM);
    EXPECT_EQ(compute_checksum(g_src, g_other, &hdr, HEADER_LEN), SYN_TO_OTHER_HOST_CHECKSUM);
}

TEST(tcp_checksum, matches_the_reference_for_an_odd_length_payload) {
    uint8_t segment[HEADER_LEN + 3];
    tcp_header hdr = header(80, 49152, 1000, 2, FLAG_PSH | FLAG_ACK, 512);
    string::memcpy(segment, &hdr, HEADER_LEN);
    string::memcpy(segment + HEADER_LEN, "abc", 3);

    EXPECT_EQ(compute_checksum(g_src, g_dst, segment, sizeof(segment)), ACK_ABC_CHECKSUM);
}

TEST(tcp_checksum, verifies_a_segment_carrying_its_own_checksum) {
    tcp_header hdr = syn_header();
    hdr.checksum = htons(compute_checksum(g_src, g_dst, &hdr, HEADER_LEN));

    EXPECT_TRUE(is_checksum_valid(g_src, g_dst, &hdr, HEADER_LEN));
}

TEST(tcp_checksum, rejects_a_changed_field_or_a_different_host) {
    tcp_header hdr = syn_header();
    hdr.checksum = htons(SYN_CHECKSUM);
    EXPECT_TRUE(is_checksum_valid(g_src, g_dst, &hdr, HEADER_LEN));

    EXPECT_FALSE(is_checksum_valid(g_src, g_other, &hdr, HEADER_LEN));

    hdr.flags = FLAG_SYN | FLAG_ACK;
    EXPECT_FALSE(is_checksum_valid(g_src, g_dst, &hdr, HEADER_LEN));

    hdr.flags = FLAG_SYN;
    hdr.seq = htonl(2);
    EXPECT_FALSE(is_checksum_valid(g_src, g_dst, &hdr, HEADER_LEN));
}

TEST(tcp_checksum, rejects_a_corrupted_payload_byte) {
    uint8_t segment[HEADER_LEN + 3];
    tcp_header hdr = header(80, 49152, 1000, 2, FLAG_PSH | FLAG_ACK, 512);
    hdr.checksum = htons(ACK_ABC_CHECKSUM);
    string::memcpy(segment, &hdr, HEADER_LEN);
    string::memcpy(segment + HEADER_LEN, "abc", 3);
    EXPECT_TRUE(is_checksum_valid(g_src, g_dst, segment, sizeof(segment)));

    segment[HEADER_LEN + 2] = 'd';
    EXPECT_FALSE(is_checksum_valid(g_src, g_dst, segment, sizeof(segment)));
}
