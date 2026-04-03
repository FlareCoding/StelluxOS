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
// Proves: the clock ticks at a reasonable rate.
// ~200 brief_delay iterations should produce a measurable delta
// that's neither zero nor absurdly large.

TEST(clock, now_ns_rate_sane) {
    uint64_t start = clock::now_ns();
    for (int i = 0; i < 200; i++) {
        brief_delay();
    }
    uint64_t end = clock::now_ns();
    uint64_t elapsed = end - start;

    EXPECT_GT(elapsed, static_cast<uint64_t>(10000000));
    EXPECT_LT(elapsed, static_cast<uint64_t>(30000000000ULL));
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
