#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "signals/signal_types.h"

TEST_SUITE(signal_types);

TEST(signal_types, sig_bit_maps_signal_to_bitmask) {
    EXPECT_EQ(signals::sig_bit(1), 1ULL);
    EXPECT_EQ(signals::sig_bit(signals::SIGKILL), 1ULL << 8);
    EXPECT_EQ(signals::sig_bit(64), 1ULL << 63);
}

TEST(signal_types, sig_valid_bounds) {
    EXPECT_FALSE(signals::sig_valid(0));
    EXPECT_TRUE(signals::sig_valid(1));
    EXPECT_TRUE(signals::sig_valid(64));
    EXPECT_FALSE(signals::sig_valid(65));
}

TEST(signal_types, unblockable_mask_is_kill_and_stop) {
    EXPECT_EQ(signals::UNBLOCKABLE_MASK,
              signals::sig_bit(signals::SIGKILL) | signals::sig_bit(signals::SIGSTOP));
}
