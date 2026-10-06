#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "trace/ktrace.h"

TEST_SUITE(ktrace);

TEST(ktrace, start_fails_while_a_session_is_recording) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(ktrace::start(), ktrace::ERR_BUSY);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

TEST(ktrace, stop_fails_when_no_session_is_recording) {
    EXPECT_EQ(ktrace::stop(), ktrace::ERR_NOT_RECORDING);
}

TEST(ktrace, a_stopped_session_can_be_started_again) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    EXPECT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}
