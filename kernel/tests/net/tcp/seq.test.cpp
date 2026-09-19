#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "net/tcp/seq.h"

TEST_SUITE(tcp_seq);

using namespace net::tcp;

constexpr uint32_t WRAP_BEFORE = 0xFFFFFFF0;
constexpr uint32_t WRAP_AFTER  = 0x00000010;
constexpr uint32_t HALF_RANGE  = 0x80000000;

// RFC 9293 3.4: RCV.NXT <= SEG.SEQ < RCV.NXT + RCV.WND
static bool in_receive_window(uint32_t seg_seq, uint32_t rcv_nxt, uint32_t rcv_wnd) {
    return seq_geq(seg_seq, rcv_nxt) && seq_lt(seg_seq, rcv_nxt + rcv_wnd);
}

TEST(tcp_seq, orders_plain_numbers) {
    EXPECT_TRUE(seq_lt(1, 2));
    EXPECT_FALSE(seq_lt(2, 1));
    EXPECT_FALSE(seq_lt(2, 2));
    EXPECT_TRUE(seq_leq(2, 2));
    EXPECT_FALSE(seq_leq(3, 2));
    EXPECT_TRUE(seq_gt(2, 1));
    EXPECT_FALSE(seq_gt(1, 2));
    EXPECT_FALSE(seq_gt(2, 2));
    EXPECT_TRUE(seq_geq(2, 2));
    EXPECT_FALSE(seq_geq(1, 2));
}

TEST(tcp_seq, orders_across_the_wrap) {
    EXPECT_TRUE(seq_lt(WRAP_BEFORE, WRAP_AFTER));
    EXPECT_FALSE(seq_lt(WRAP_AFTER, WRAP_BEFORE));
    EXPECT_TRUE(seq_leq(WRAP_BEFORE, WRAP_AFTER));
    EXPECT_TRUE(seq_gt(WRAP_AFTER, WRAP_BEFORE));
    EXPECT_FALSE(seq_gt(WRAP_BEFORE, WRAP_AFTER));
    EXPECT_TRUE(seq_geq(WRAP_AFTER, WRAP_BEFORE));
    EXPECT_TRUE(seq_lt(0xFFFFFFFF, 0));
    EXPECT_TRUE(seq_gt(0, 0xFFFFFFFF));
}

TEST(tcp_seq, orders_any_distance_below_half_the_range) {
    EXPECT_TRUE(seq_lt(0, HALF_RANGE - 1));
    EXPECT_TRUE(seq_gt(HALF_RANGE - 1, 0));
    EXPECT_TRUE(seq_lt(HALF_RANGE, 0xFFFFFFFF));
    EXPECT_TRUE(seq_lt(0x12345678, 0x12345678 + HALF_RANGE - 1));
    EXPECT_TRUE(seq_gt(0x12345678 + HALF_RANGE - 1, 0x12345678));
}

TEST(tcp_seq, between_includes_both_ends) {
    EXPECT_TRUE(seq_between(10, 10, 20));
    EXPECT_TRUE(seq_between(15, 10, 20));
    EXPECT_TRUE(seq_between(20, 10, 20));
    EXPECT_FALSE(seq_between(9, 10, 20));
    EXPECT_FALSE(seq_between(21, 10, 20));
    EXPECT_TRUE(seq_between(10, 10, 10));
    EXPECT_FALSE(seq_between(11, 10, 10));
}

TEST(tcp_seq, between_follows_a_range_across_the_wrap) {
    EXPECT_TRUE(seq_between(WRAP_BEFORE, WRAP_BEFORE, WRAP_AFTER));
    EXPECT_TRUE(seq_between(0xFFFFFFFF, WRAP_BEFORE, WRAP_AFTER));
    EXPECT_TRUE(seq_between(0, WRAP_BEFORE, WRAP_AFTER));
    EXPECT_TRUE(seq_between(WRAP_AFTER, WRAP_BEFORE, WRAP_AFTER));
    EXPECT_FALSE(seq_between(WRAP_BEFORE - 1, WRAP_BEFORE, WRAP_AFTER));
    EXPECT_FALSE(seq_between(WRAP_AFTER + 1, WRAP_BEFORE, WRAP_AFTER));
    EXPECT_FALSE(seq_between(HALF_RANGE, WRAP_BEFORE, WRAP_AFTER));
}

TEST(tcp_seq, expresses_the_receive_window_test) {
    uint32_t rcv_nxt = 0xFFFFFF00;
    uint32_t rcv_wnd = 0x200;

    EXPECT_TRUE(in_receive_window(rcv_nxt, rcv_nxt, rcv_wnd));
    EXPECT_TRUE(in_receive_window(0xFFFFFFFF, rcv_nxt, rcv_wnd));
    EXPECT_TRUE(in_receive_window(0, rcv_nxt, rcv_wnd));
    EXPECT_TRUE(in_receive_window(0xFF, rcv_nxt, rcv_wnd));
    EXPECT_FALSE(in_receive_window(0x100, rcv_nxt, rcv_wnd));
    EXPECT_FALSE(in_receive_window(rcv_nxt - 1, rcv_nxt, rcv_wnd));
    EXPECT_FALSE(in_receive_window(rcv_nxt, rcv_nxt, 0));
}
