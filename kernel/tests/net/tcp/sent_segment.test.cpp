#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "net/tcp/sent_segment.h"
#include "net/tcp/byte_queue.h"

TEST_SUITE(tcp_sent_segment);

using namespace net::tcp;

constexpr uint64_t T0 = 1000000000ULL;
constexpr uint64_t MS = 1000000ULL;

// A record list that gives everything back when the test ends
struct scoped_segments {
    sent_segments s;

    explicit scoped_segments(size_t cap) { s.init(cap); }
    ~scoped_segments() { s.clear(); }
};

TEST(tcp_sent_segment, tracks_segments_in_order_and_charges_the_budget) {
    size_t budget_before = budget_in_use();
    scoped_segments r(8);
    EXPECT_TRUE(r.s.empty());

    sent_segment* first = r.s.track(1000, 2000, T0);
    sent_segment* second = r.s.track(2000, 2500, T0 + MS);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    EXPECT_EQ(r.s.count(), 2u);
    EXPECT_EQ(r.s.oldest(), first);
    EXPECT_EQ(r.s.newest(), second);
    EXPECT_EQ(first->start_seq, 1000u);
    EXPECT_EQ(first->end_seq, 2000u);
    EXPECT_EQ(first->sent_ns, T0);
    EXPECT_EQ(first->retrans, 0);
    EXPECT_EQ(first->marks, 0);
    EXPECT_EQ(budget_in_use(), budget_before + 2 * SENT_SEGMENT_COST);
}

TEST(tcp_sent_segment, an_acknowledgment_frees_whole_records_and_trims_a_partial_one) {
    size_t budget_before = budget_in_use();
    scoped_segments r(8);
    r.s.track(1000, 2000, T0);
    r.s.track(2000, 3000, T0 + MS);
    r.s.track(3000, 4000, T0 + 2 * MS);

    acknowledged a = r.s.acknowledge(2500);
    EXPECT_EQ(a.bytes, 1500u);
    EXPECT_EQ(a.rtt_sample_sent_ns, T0);
    EXPECT_EQ(r.s.count(), 2u);
    EXPECT_EQ(r.s.oldest()->start_seq, 2500u);
    EXPECT_EQ(r.s.oldest()->end_seq, 3000u);
    EXPECT_EQ(budget_in_use(), budget_before + 2 * SENT_SEGMENT_COST);

    a = r.s.acknowledge(2500);
    EXPECT_EQ(a.bytes, 0u);
    EXPECT_EQ(a.rtt_sample_sent_ns, 0u);

    a = r.s.acknowledge(1500);
    EXPECT_EQ(a.bytes, 0u);
    EXPECT_EQ(r.s.count(), 2u);

    a = r.s.acknowledge(4000);
    EXPECT_EQ(a.bytes, 1500u);
    EXPECT_EQ(a.rtt_sample_sent_ns, T0 + MS);
    EXPECT_TRUE(r.s.empty());
    EXPECT_EQ(budget_in_use(), budget_before);
}

TEST(tcp_sent_segment, a_retransmitted_record_yields_no_rtt_sample) {
    scoped_segments r(8);
    sent_segment* first = r.s.track(1000, 2000, T0);
    r.s.track(2000, 3000, T0 + MS);

    r.s.mark_retransmitted(first, T0 + 5 * MS);
    EXPECT_EQ(first->retrans, 1);
    EXPECT_EQ(first->sent_ns, T0 + 5 * MS);
    EXPECT_EQ(first->marks, MARK_RETRANSMITTED);

    acknowledged a = r.s.acknowledge(2000);
    EXPECT_EQ(a.bytes, 1000u);
    EXPECT_EQ(a.rtt_sample_sent_ns, 0u);

    a = r.s.acknowledge(3000);
    EXPECT_EQ(a.bytes, 1000u);
    EXPECT_EQ(a.rtt_sample_sent_ns, T0 + MS);
}

TEST(tcp_sent_segment, the_sample_comes_from_the_oldest_record_covered_that_was_never_retransmitted) {
    scoped_segments r(8);
    sent_segment* first = r.s.track(1000, 2000, T0);
    r.s.track(2000, 3000, T0 + MS);
    r.s.track(3000, 4000, T0 + 2 * MS);
    r.s.mark_retransmitted(first, T0 + 9 * MS);

    acknowledged a = r.s.acknowledge(4000);
    EXPECT_EQ(a.bytes, 3000u);
    EXPECT_EQ(a.rtt_sample_sent_ns, T0 + MS);
}

TEST(tcp_sent_segment, the_cap_and_the_budget_refuse_further_records) {
    size_t budget_before = budget_in_use();
    scoped_segments r(2);
    ASSERT_NOT_NULL(r.s.track(1000, 2000, T0));
    ASSERT_NOT_NULL(r.s.track(2000, 3000, T0));
    EXPECT_EQ(r.s.track(3000, 4000, T0), nullptr);
    EXPECT_EQ(r.s.count(), 2u);

    r.s.set_cap(3);
    __dbg_test_set_budget(budget_in_use());
    EXPECT_EQ(r.s.track(3000, 4000, T0), nullptr);
    EXPECT_EQ(budget_in_use(), budget_before + 2 * SENT_SEGMENT_COST);

    __dbg_test_set_budget(GLOBAL_BUDGET);
    ASSERT_NOT_NULL(r.s.track(3000, 4000, T0));
    EXPECT_EQ(r.s.count(), 3u);
}

TEST(tcp_sent_segment, sequence_numbers_wrap_past_zero) {
    scoped_segments r(8);
    r.s.track(0xFFFFFF00u, 0xFFFFFFC0u, T0);
    r.s.track(0xFFFFFFC0u, 0x00000040u, T0 + MS);
    r.s.track(0x00000040u, 0x00000100u, T0 + 2 * MS);

    acknowledged a = r.s.acknowledge(0x00000010u);
    EXPECT_EQ(a.bytes, 0x110u);
    EXPECT_EQ(a.rtt_sample_sent_ns, T0);
    EXPECT_EQ(r.s.count(), 2u);
    EXPECT_EQ(r.s.oldest()->start_seq, 0x00000010u);

    a = r.s.acknowledge(0x00000100u);
    EXPECT_EQ(a.bytes, 0xF0u);
    EXPECT_TRUE(r.s.empty());
}

TEST(tcp_sent_segment, clear_returns_every_record) {
    size_t budget_before = budget_in_use();
    scoped_segments r(8);
    r.s.track(1000, 2000, T0);
    r.s.track(2000, 3000, T0);

    r.s.clear();
    EXPECT_TRUE(r.s.empty());
    EXPECT_EQ(r.s.oldest(), nullptr);
    EXPECT_EQ(budget_in_use(), budget_before);
    ASSERT_NOT_NULL(r.s.track(5000, 6000, T0));
}
