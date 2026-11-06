#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "clock/clock.h"
#include "hw/delay.h"

using test_helpers::brief_delay;

TEST_SUITE(clock);

// --- now_ns_monotonic ---
// Proves: two consecutive calls return non-decreasing values.

TEST(clock, now_ns_monotonic) {
    uint64_t t1 = clock::now_ns();
    uint64_t t2 = clock::now_ns();
    EXPECT_LE(t1, t2);
}

// --- now_ns_advances ---
// Proves: after a busy-wait loop, the clock has advanced.

TEST(clock, now_ns_advances) {
    uint64_t t1 = clock::now_ns();
    brief_delay();
    uint64_t t2 = clock::now_ns();
    EXPECT_GT(t2, t1);
}

// --- now_ns_rate_sane ---
// Proves: the clock ticks at a sane rate, timing 20 `brief_delay` calls at more than 1 ms and less than 3 s.

TEST(clock, now_ns_rate_sane) {
    uint64_t start = clock::now_ns();
    for (int i = 0; i < 20; i++) {
        brief_delay();
    }
    uint64_t end = clock::now_ns();
    uint64_t elapsed = end - start;

    EXPECT_GT(elapsed, static_cast<uint64_t>(1000000));
    EXPECT_LT(elapsed, static_cast<uint64_t>(3000000000ULL));
}

constexpr uint64_t SECOND_NS = 1000000000ULL;

// A timeout whose sum with now_ns() stays below UINT64_MAX gives a deadline that long after the call.
TEST(clock, deadline_after_ns_adds_the_timeout_to_now) {
    uint64_t before = clock::now_ns();
    uint64_t in_a_second = clock::deadline_after_ns(SECOND_NS);
    uint64_t at_once = clock::deadline_after_ns(0);
    uint64_t after = clock::now_ns();

    EXPECT_GE(in_a_second, before + SECOND_NS);
    EXPECT_LE(in_a_second, after + SECOND_NS);
    EXPECT_GE(at_once, before);
    EXPECT_LE(at_once, after);
}

// A timeout whose sum with now_ns() reaches or passes UINT64_MAX gives UINT64_MAX, and one whose sum falls a second
// short of UINT64_MAX gives the sum.
TEST(clock, deadline_after_ns_saturates_at_uint64_max) {
    uint64_t room = ~0ULL - clock::now_ns();
    uint64_t a_second_short = clock::deadline_after_ns(room - SECOND_NS);

    EXPECT_EQ(clock::deadline_after_ns(~0ULL), ~0ULL);
    EXPECT_EQ(clock::deadline_after_ns(room), ~0ULL);
    EXPECT_GE(a_second_short, ~0ULL - SECOND_NS);
    EXPECT_LT(a_second_short, ~0ULL);
}

// --- delay_us_accuracy ---
// Proves: delay::us(1000) produces ~1ms elapsed (wide tolerance for QEMU).

TEST(clock, delay_us_accuracy) {
    uint64_t start = clock::now_ns();
    delay::us(1000);
    uint64_t end = clock::now_ns();
    uint64_t elapsed = end - start;

    EXPECT_GE(elapsed, static_cast<uint64_t>(500000));
    EXPECT_LE(elapsed, static_cast<uint64_t>(50000000));
}

// --- delay_ns_zero ---
// Proves: delay::ns(0) returns immediately.

TEST(clock, delay_ns_zero) {
    uint64_t start = clock::now_ns();
    delay::ns(0);
    uint64_t end = clock::now_ns();
    uint64_t elapsed = end - start;

    EXPECT_LT(elapsed, static_cast<uint64_t>(1000000));
}
