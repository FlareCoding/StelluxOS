#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "net/tcp/conn.h"
#include "net/tcp/seq.h"
#include "clock/clock.h"

TEST_SUITE(tcp_isn);

using namespace net;
using namespace net::tcp;

static const tuple g_key = {{{10, 0, 2, 15}}, {{10, 0, 2, 2}}, 5000, 40000};

static void spin_for_ns(uint64_t ns) {
    uint64_t until = clock::now_ns() + ns;
    while (clock::now_ns() < until) {
    }
}

TEST(tcp_isn, one_tuple_advances_with_the_clock) {
    uint32_t earlier = initial_sequence(g_key);
    spin_for_ns(20 * ISN_TICK_NS);
    uint32_t later = initial_sequence(g_key);

    EXPECT_TRUE(seq_gt(later, earlier));
    EXPECT_TRUE(later - earlier >= 20);
    EXPECT_TRUE(later - earlier < 250000);
}

TEST(tcp_isn, every_field_of_the_tuple_changes_the_number) {
    uint32_t base = initial_sequence(g_key);

    tuple other_local_port = g_key;
    other_local_port.local_port = 5001;
    tuple other_remote_port = g_key;
    other_remote_port.remote_port = 40001;
    tuple other_local_addr = g_key;
    other_local_addr.local_addr = {{10, 0, 2, 16}};
    tuple other_remote_addr = g_key;
    other_remote_addr.remote_addr = {{10, 0, 2, 3}};

    // Taken within microseconds, the numbers differ by their hashes alone,
    // which agree with odds of one in four billion
    EXPECT_TRUE(initial_sequence(other_local_port) != base);
    EXPECT_TRUE(initial_sequence(other_remote_port) != base);
    EXPECT_TRUE(initial_sequence(other_local_addr) != base);
    EXPECT_TRUE(initial_sequence(other_remote_addr) != base);
}

TEST(tcp_isn, both_ends_of_one_connection_get_different_numbers) {
    tuple reversed = {g_key.remote_addr, g_key.local_addr, g_key.remote_port, g_key.local_port};

    EXPECT_TRUE(initial_sequence(reversed) != initial_sequence(g_key));
}

TEST(tcp_isn, timestamp_offset_is_fixed_per_tuple_and_differs_between_tuples) {
    tuple other = g_key;
    other.remote_port = 40001;

    EXPECT_EQ(timestamp_offset(g_key), timestamp_offset(g_key));
    EXPECT_TRUE(timestamp_offset(g_key) != timestamp_offset(other));
}

TEST(tcp_isn, timestamp_offset_and_sequence_number_use_different_secrets) {
    uint32_t sequence_hash = initial_sequence(g_key) - static_cast<uint32_t>(clock::now_ns() / ISN_TICK_NS);

    EXPECT_TRUE(sequence_hash != timestamp_offset(g_key));
}
