// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/core_methods.h"

TEST_SUITE(nvidia_core_methods);

using drivers::nvidia::core_initial_method;
using drivers::nvidia::display_method;

// NVIDIA's methods for head 0, the stride between heads, a coefficient's DATA mask and INDEX shift and an opaque ALPHA,
// restated so the tests hold the methods to NVIDIA's values rather than to the driver's own
static constexpr uint32_t NVIDIA_SCALER_COEFFICIENT = 0x2214;
static constexpr uint32_t NVIDIA_ALPHA_RED          = 0x2220;
static constexpr uint32_t NVIDIA_GREEN_BLUE         = 0x2224;
static constexpr uint32_t NVIDIA_HEAD_STRIDE        = 0x400;
static constexpr uint32_t NVIDIA_DATA_MASK          = 0x3FF;
static constexpr uint32_t NVIDIA_INDEX_SHIFT        = 12;
static constexpr uint32_t NVIDIA_OPAQUE_ALPHA       = 255;

// NVIDIA's coefficients, 3 ratios of 16 phases of 4 values, each head's methods, those coefficients and the desktop
// color's two, and GA102's heads, the chip table's four
static constexpr uint32_t NVIDIA_RATIOS             = 3;
static constexpr uint32_t NVIDIA_RATIO_COEFFICIENTS = 16 * 4;
static constexpr uint32_t NVIDIA_COEFFICIENTS       = NVIDIA_RATIOS * NVIDIA_RATIO_COEFFICIENTS;
static constexpr uint32_t NVIDIA_METHODS_PER_HEAD   = NVIDIA_COEFFICIENTS + 2;
static constexpr uint8_t  GA102_HEADS               = 4;

// A method no call gives, so a call returning false is seen to leave it unchanged
static constexpr display_method UNTOUCHED = {0xDEAD, 0xBEEF};

// For one to four heads the methods end after every coefficient and both desktop color methods of each head, and a
// call past the end leaves its method unchanged
TEST(nvidia_core_methods, methods_end_after_the_coefficients_and_desktop_colors_of_each_head) {
    for (uint8_t heads = 1; heads <= GA102_HEADS; heads++) {
        uint32_t end = NVIDIA_METHODS_PER_HEAD * heads;
        display_method last = {};
        display_method past_end = UNTOUCHED;

        EXPECT_TRUE(core_initial_method(heads, end - 1, last));
        EXPECT_FALSE(core_initial_method(heads, end, past_end));
        EXPECT_EQ(past_end.method, UNTOUCHED.method);
        EXPECT_EQ(past_end.value, UNTOUCHED.value);
    }
}

// Each coefficient goes into every head from head 0 before the next one, as the same value one head gets, with its
// position in the table as its INDEX
TEST(nvidia_core_methods, each_coefficient_goes_into_every_head_before_the_next) {
    for (uint32_t index = 0; index < NVIDIA_COEFFICIENTS * GA102_HEADS; index++) {
        uint32_t coefficient = index / GA102_HEADS;
        uint32_t head = index % GA102_HEADS;
        display_method method = {};
        display_method one_head = {};

        ASSERT_TRUE(core_initial_method(GA102_HEADS, index, method));
        ASSERT_TRUE(core_initial_method(1, coefficient, one_head));
        EXPECT_EQ(method.method, NVIDIA_SCALER_COEFFICIENT + head * NVIDIA_HEAD_STRIDE);
        EXPECT_EQ(method.value, one_head.value);
        EXPECT_EQ(method.value >> NVIDIA_INDEX_SHIFT, coefficient);
    }
}

// A coefficient's DATA is NVIDIA's value in ten bits, a negative one in two's complement
TEST(nvidia_core_methods, coefficients_carry_nvidias_values_in_ten_bits) {
    struct coefficient {
        uint32_t index;
        uint32_t data;
    };

    // Ratio 1's first row, the last value of its last phase, ratio 2's first value and ratio 4's last value, NVIDIA's
    // 0, 0, -16, 144, -15, 3 and 22
    const coefficient coefficients[] = {
        {0, 0},
        {1, 0},
        {2, 0x3F0},
        {3, 144},
        {63, 0x3F1},
        {64, 3},
        {191, 22},
    };

    for (const coefficient& expected : coefficients) {
        display_method method = {};
        ASSERT_TRUE(core_initial_method(1, expected.index, method));
        EXPECT_EQ(method.value, expected.data | expected.index << NVIDIA_INDEX_SHIFT);
    }
}

// Each ratio's 64 coefficients from NVIDIA's table in ten bits, each times its position in the ratio counted from 1,
// summed, so a changed or moved coefficient changes its ratio's sum
static constexpr uint32_t NVIDIA_RATIO_SUMS[NVIDIA_RATIOS] = {1578987, 71774, 74154};

// Every coefficient carries its position in the table in INDEX and nothing else outside DATA, and each ratio's DATA
// values give NVIDIA's position-weighted sum
TEST(nvidia_core_methods, coefficients_carry_their_indexes_and_nvidias_ratio_sums) {
    for (uint32_t ratio = 0; ratio < NVIDIA_RATIOS; ratio++) {
        uint32_t sum = 0;
        for (uint32_t position = 0; position < NVIDIA_RATIO_COEFFICIENTS; position++) {
            uint32_t coefficient = ratio * NVIDIA_RATIO_COEFFICIENTS + position;
            display_method method = {};
            ASSERT_TRUE(core_initial_method(1, coefficient, method));

            EXPECT_EQ(method.value & ~NVIDIA_DATA_MASK, coefficient << NVIDIA_INDEX_SHIFT);
            sum += (position + 1) * (method.value & NVIDIA_DATA_MASK);
        }

        EXPECT_EQ(sum, NVIDIA_RATIO_SUMS[ratio]);
    }
}

// After the coefficients each head in turn gets its desktop color, opaque black, ALPHA_RED's method before GREEN_BLUE's
TEST(nvidia_core_methods, each_head_then_gets_an_opaque_black_desktop_color) {
    for (uint8_t head = 0; head < GA102_HEADS; head++) {
        uint32_t index = NVIDIA_COEFFICIENTS * GA102_HEADS + head * 2;
        display_method alpha_red = {};
        display_method green_blue = {};

        ASSERT_TRUE(core_initial_method(GA102_HEADS, index, alpha_red));
        ASSERT_TRUE(core_initial_method(GA102_HEADS, index + 1, green_blue));
        EXPECT_EQ(alpha_red.method, NVIDIA_ALPHA_RED + head * NVIDIA_HEAD_STRIDE);
        EXPECT_EQ(alpha_red.value, NVIDIA_OPAQUE_ALPHA);
        EXPECT_EQ(green_blue.method, NVIDIA_GREEN_BLUE + head * NVIDIA_HEAD_STRIDE);
        EXPECT_EQ(green_blue.value, 0u);
    }
}
