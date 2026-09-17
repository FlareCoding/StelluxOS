#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "net/tcp/wire.h"
#include "common/string.h"

TEST_SUITE(tcp_options);

using namespace net::tcp;

struct segment {
    alignas(4) uint8_t bytes[MAX_HEADER_LEN];

    const tcp_header* hdr() const { return reinterpret_cast<const tcp_header*>(bytes); }
};

template <typename... Byte>
static segment with_options(uint8_t flags, Byte... bytes) {
    constexpr size_t opts_len = sizeof...(bytes);
    static_assert(opts_len % sizeof(uint32_t) == 0, "options must fill whole words");

    segment seg = {};
    tcp_header* hdr = reinterpret_cast<tcp_header*>(seg.bytes);
    hdr->flags = flags;
    hdr->set_data_offset(static_cast<uint8_t>((HEADER_LEN + opts_len) / sizeof(uint32_t)));

    if constexpr (opts_len > 0) {
        const uint8_t opts[] = {static_cast<uint8_t>(bytes)...};
        string::memcpy(seg.bytes + HEADER_LEN, opts, opts_len);
    }

    return seg;
}

static void expect_absent(const tcp_options& opts) {
    EXPECT_EQ(opts.mss, MSS_NONE);
    EXPECT_EQ(opts.window_scale, WINDOW_SCALE_NONE);
    EXPECT_FALSE(opts.sack_permitted);
    EXPECT_FALSE(opts.has_timestamps);
    EXPECT_EQ(opts.sack_count, 0);
}

TEST(tcp_options, bare_header_has_no_options) {
    segment seg = with_options(FLAG_SYN);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    expect_absent(opts);
}

TEST(tcp_options, parses_the_full_syn_layout) {
    segment seg = with_options(FLAG_SYN,
        OPT_MSS, 4, 0x05, 0xB4,
        OPT_SACK_PERMITTED, 2,
        OPT_TIMESTAMPS, 10, 0x00, 0x00, 0x30, 0x39, 0x00, 0x00, 0x00, 0x00,
        OPT_NOP,
        OPT_WINDOW_SCALE, 3, 7);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_EQ(opts.mss, 1460);
    EXPECT_TRUE(opts.sack_permitted);
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.ts_val, 12345u);
    EXPECT_EQ(opts.ts_ecr, 0u);
    EXPECT_EQ(opts.window_scale, 7);
    EXPECT_EQ(opts.sack_count, 0);
}

TEST(tcp_options, syn_only_options_are_ignored_without_syn) {
    segment seg = with_options(FLAG_ACK,
        OPT_MSS, 4, 0x05, 0xB4,
        OPT_SACK_PERMITTED, 2,
        OPT_TIMESTAMPS, 10, 0x00, 0x00, 0x30, 0x39, 0x00, 0x00, 0x00, 0x07,
        OPT_NOP,
        OPT_WINDOW_SCALE, 3, 7);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_EQ(opts.mss, MSS_NONE);
    EXPECT_FALSE(opts.sack_permitted);
    EXPECT_EQ(opts.window_scale, WINDOW_SCALE_NONE);
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.ts_val, 12345u);
    EXPECT_EQ(opts.ts_ecr, 7u);
}

TEST(tcp_options, parses_timestamps_and_sack_blocks_in_host_order) {
    segment seg = with_options(FLAG_ACK,
        OPT_NOP, OPT_NOP,
        OPT_TIMESTAMPS, 10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
        OPT_SACK, 18,
        0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00,
        0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x40, 0x00,
        OPT_END, OPT_END);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.ts_val, 1u);
    EXPECT_EQ(opts.ts_ecr, 2u);
    EXPECT_EQ(opts.sack_count, 2);
    EXPECT_EQ(opts.sack_blocks[0].start, 0x1000u);
    EXPECT_EQ(opts.sack_blocks[0].end, 0x2000u);
    EXPECT_EQ(opts.sack_blocks[1].start, 0x3000u);
    EXPECT_EQ(opts.sack_blocks[1].end, 0x4000u);
}

TEST(tcp_options, parses_four_sack_blocks) {
    segment seg = with_options(FLAG_ACK,
        OPT_NOP, OPT_NOP,
        OPT_SACK, 34,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x08);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_EQ(opts.sack_count, 4);
    EXPECT_EQ(opts.sack_blocks[3].start, 7u);
    EXPECT_EQ(opts.sack_blocks[3].end, 8u);
}

TEST(tcp_options, clamps_the_window_scale_to_fourteen) {
    tcp_options opts;

    segment over = with_options(FLAG_SYN, OPT_WINDOW_SCALE, 3, 15, OPT_END);
    EXPECT_TRUE(parse_options(over.hdr(), &opts));
    EXPECT_EQ(opts.window_scale, MAX_WINDOW_SCALE);

    segment limit = with_options(FLAG_SYN, OPT_WINDOW_SCALE, 3, 14, OPT_END);
    EXPECT_TRUE(parse_options(limit.hdr(), &opts));
    EXPECT_EQ(opts.window_scale, 14);

    segment zero = with_options(FLAG_SYN, OPT_WINDOW_SCALE, 3, 0, OPT_END);
    EXPECT_TRUE(parse_options(zero.hdr(), &opts));
    EXPECT_EQ(opts.window_scale, 0);
}

TEST(tcp_options, skips_unknown_kinds_by_their_length) {
    segment seg = with_options(FLAG_SYN,
        30, 6, 0xAA, 0xBB, 0xCC, 0xDD,
        OPT_MSS, 4, 0x02, 0x18,
        OPT_END, OPT_END, OPT_END, OPT_END, OPT_END, OPT_END);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_EQ(opts.mss, 536);
}

TEST(tcp_options, skips_a_known_kind_with_the_wrong_length) {
    segment seg = with_options(FLAG_SYN,
        OPT_MSS, 3, 0x05,
        OPT_WINDOW_SCALE, 4, 7, 0,
        OPT_SACK, 7, 0, 0, 0, 0, 0,
        OPT_TIMESTAMPS, 10, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00,
        OPT_END, OPT_END, OPT_END, OPT_END, OPT_END, OPT_END, OPT_END, OPT_END);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_EQ(opts.mss, MSS_NONE);
    EXPECT_EQ(opts.window_scale, WINDOW_SCALE_NONE);
    EXPECT_EQ(opts.sack_count, 0);
    EXPECT_TRUE(opts.has_timestamps);
    EXPECT_EQ(opts.ts_val, 9u);
}

TEST(tcp_options, end_stops_parsing_whatever_follows) {
    segment seg = with_options(FLAG_SYN,
        OPT_MSS, 4, 0x05, 0xB4,
        OPT_END,
        OPT_TIMESTAMPS, 10, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00,
        0xFF);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_EQ(opts.mss, 1460);
    EXPECT_FALSE(opts.has_timestamps);
}

TEST(tcp_options, zero_mss_reads_as_absent) {
    segment seg = with_options(FLAG_SYN, OPT_MSS, 4, 0x00, 0x00);
    tcp_options opts;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    EXPECT_EQ(opts.mss, MSS_NONE);
}

TEST(tcp_options, rejects_a_length_below_two) {
    tcp_options opts;

    segment zero_len = with_options(FLAG_SYN, OPT_MSS, 0, 0x05, 0xB4);
    EXPECT_FALSE(parse_options(zero_len.hdr(), &opts));

    segment one_len = with_options(FLAG_SYN, OPT_MSS, 1, 0x05, 0xB4);
    EXPECT_FALSE(parse_options(one_len.hdr(), &opts));

    segment unknown_zero_len = with_options(FLAG_SYN, 30, 0, 0, 0);
    EXPECT_FALSE(parse_options(unknown_zero_len.hdr(), &opts));
}

TEST(tcp_options, rejects_an_option_running_past_the_header) {
    tcp_options opts;

    segment truncated_mss = with_options(FLAG_SYN, OPT_MSS, 10, 0x05, 0xB4);
    EXPECT_FALSE(parse_options(truncated_mss.hdr(), &opts));

    segment truncated_ts = with_options(FLAG_ACK, OPT_TIMESTAMPS, 10, 0, 0, 0, 1, 0, 0);
    EXPECT_FALSE(parse_options(truncated_ts.hdr(), &opts));

    segment truncated_unknown = with_options(FLAG_ACK, OPT_NOP, OPT_NOP, 30, 3);
    EXPECT_FALSE(parse_options(truncated_unknown.hdr(), &opts));
}

TEST(tcp_options, rejects_a_kind_with_no_length_byte) {
    segment seg = with_options(FLAG_SYN, OPT_NOP, OPT_NOP, OPT_NOP, OPT_MSS);
    tcp_options opts;

    EXPECT_FALSE(parse_options(seg.hdr(), &opts));
}

TEST(tcp_options, resets_the_output_before_parsing) {
    segment seg = with_options(FLAG_ACK, OPT_NOP, OPT_NOP, OPT_NOP, OPT_NOP);
    tcp_options opts;
    opts.mss = 1460;
    opts.sack_count = 3;
    opts.has_timestamps = true;

    EXPECT_TRUE(parse_options(seg.hdr(), &opts));
    expect_absent(opts);
}
