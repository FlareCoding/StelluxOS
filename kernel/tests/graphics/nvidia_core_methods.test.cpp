// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/core_methods.h"

TEST_SUITE(nvidia_core_methods);

using drivers::nvidia::active_heads;
using drivers::nvidia::build_head_light_methods;
using drivers::nvidia::build_shutdown_methods;
using drivers::nvidia::build_window_usage_methods;
using drivers::nvidia::core_initial_method;
using drivers::nvidia::display_heads;
using drivers::nvidia::display_method;
using drivers::nvidia::DISPLAY_WINDOWS_MAX;
using drivers::nvidia::head_light;
using drivers::nvidia::head_light_methods;
using drivers::nvidia::NO_HEAD;
using drivers::nvidia::shutdown_methods;
using drivers::nvidia::window_usage_methods;

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

// NVIDIA's window, link, cursor and display id methods for window, link or head 0, their strides, its no-owner value,
// its default usage bounds and its disabled cursor's control
static constexpr uint32_t NVIDIA_WINDOW_CONTROL  = 0x1000;
static constexpr uint32_t NVIDIA_USAGE_BOUNDS    = 0x1010;
static constexpr uint32_t NVIDIA_WINDOW_STRIDE   = 0x80;
static constexpr uint32_t NVIDIA_LINK_CONTROL    = 0x300;
static constexpr uint32_t NVIDIA_LINK_STRIDE     = 0x20;
static constexpr uint32_t NVIDIA_CURSOR_CONTEXT  = 0x2088;
static constexpr uint32_t NVIDIA_CURSOR_CONTROL  = 0x209C;
static constexpr uint32_t NVIDIA_DISPLAY_ID      = 0x2020;
static constexpr uint32_t NVIDIA_OWNER_NONE      = 0xF;
static constexpr uint32_t NVIDIA_DEFAULT_BOUNDS  = 0x10110000;
static constexpr uint32_t NVIDIA_CURSOR_DISABLED = 0xCF;

// Windows 0 to 4 serve heads 0, 0, 1, 1 and none, window 3 is not present, and head 1 shows display id 0x100 on serial
// link 2, NVIDIA's link type 2 being a serial link (SOR)
static constexpr uint32_t PRESENT_WINDOWS  = 0x17;
static constexpr uint32_t SHOWN_DISPLAY    = 0x100;
static constexpr uint32_t SHOWN_LINK       = 2;
static constexpr uint32_t SERIAL_LINK_TYPE = 2;

// The methods stopping head 1 on link 2, and the window mapping of the present windows before them
static constexpr display_method HEAD_1_STOPPED[] = {
    {NVIDIA_CURSOR_CONTEXT + NVIDIA_HEAD_STRIDE, 0},
    {NVIDIA_CURSOR_CONTROL + NVIDIA_HEAD_STRIDE, NVIDIA_CURSOR_DISABLED},
    {NVIDIA_LINK_CONTROL + SHOWN_LINK * NVIDIA_LINK_STRIDE, 0},
    {NVIDIA_DISPLAY_ID + NVIDIA_HEAD_STRIDE, 0},
};

static constexpr display_method WINDOW_MAPPING[] = {
    {NVIDIA_WINDOW_CONTROL, 0},
    {NVIDIA_WINDOW_CONTROL + NVIDIA_WINDOW_STRIDE, 0},
    {NVIDIA_WINDOW_CONTROL + 2 * NVIDIA_WINDOW_STRIDE, 1},
    {NVIDIA_WINDOW_CONTROL + 4 * NVIDIA_WINDOW_STRIDE, NVIDIA_OWNER_NONE},
    {NVIDIA_USAGE_BOUNDS, NVIDIA_DEFAULT_BOUNDS},
    {NVIDIA_USAGE_BOUNDS + NVIDIA_WINDOW_STRIDE, NVIDIA_DEFAULT_BOUNDS},
    {NVIDIA_USAGE_BOUNDS + 2 * NVIDIA_WINDOW_STRIDE, NVIDIA_DEFAULT_BOUNDS},
    {NVIDIA_USAGE_BOUNDS + 4 * NVIDIA_WINDOW_STRIDE, NVIDIA_DEFAULT_BOUNDS},
};

// Gives two heads, with windows 0 and 1 serving head 0, windows 2 and 3 head 1 and every other window none
static display_heads shutdown_heads() {
    display_heads heads = {};
    heads.count = 2;
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        heads.window_head[window] = NO_HEAD;
    }

    heads.window_head[0] = 0;
    heads.window_head[1] = 0;
    heads.window_head[2] = 1;
    heads.window_head[3] = 1;
    return heads;
}

// Gives head 1 alone active, showing `SHOWN_DISPLAY` through serial link `SHOWN_LINK`
static active_heads head_1_active() {
    active_heads active = {};
    active.heads[1] = {.display_id = SHOWN_DISPLAY, .link_index = SHOWN_LINK, .link_type = SERIAL_LINK_TYPE};
    return active;
}

// Expects the `count` methods of `methods` from index `first` on to equal `expected`
template <typename method_list>
static void expect_methods(
    const method_list& methods,
    uint32_t first,
    const display_method* expected,
    uint32_t count
) {
    for (uint32_t index = 0; index < count; index++) {
        EXPECT_EQ(methods.methods[first + index].method, expected[index].method);
        EXPECT_EQ(methods.methods[first + index].value, expected[index].value);
    }
}

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

// The first update gives every present window its head or none before giving any NVIDIA's default usage bounds,
// skipping window 3, then stops head 1: its cursor disabled, its link carrying no head and its display id cleared
TEST(nvidia_core_methods, first_update_maps_the_present_windows_before_stopping_the_heads) {
    shutdown_methods methods = {};

    build_shutdown_methods(shutdown_heads(), PRESENT_WINDOWS, head_1_active(), true, methods);

    ASSERT_EQ(methods.count, 12u);
    expect_methods(methods, 0, WINDOW_MAPPING, 8);
    expect_methods(methods, 8, HEAD_1_STOPPED, 4);
}

// A later update only stops the active heads, and an inactive head 0 gets no method
TEST(nvidia_core_methods, later_update_only_stops_the_active_heads) {
    shutdown_methods methods = {};

    build_shutdown_methods(shutdown_heads(), PRESENT_WINDOWS, head_1_active(), false, methods);

    ASSERT_EQ(methods.count, 4u);
    expect_methods(methods, 0, HEAD_1_STOPPED, 4);
}

// Heads 0 and 2 sharing serial link 1 are stopped in ascending order, each setting the link to carry no head
TEST(nvidia_core_methods, heads_sharing_a_link_each_set_it_to_carry_no_head) {
    active_heads active = {};
    active.heads[0] = {.display_id = SHOWN_DISPLAY, .link_index = 1, .link_type = SERIAL_LINK_TYPE};
    active.heads[2] = {.display_id = SHOWN_DISPLAY << 1, .link_index = 1, .link_type = SERIAL_LINK_TYPE};
    shutdown_methods methods = {};

    build_shutdown_methods(shutdown_heads(), PRESENT_WINDOWS, active, false, methods);

    ASSERT_EQ(methods.count, 8u);
    EXPECT_EQ(methods.methods[2].method, NVIDIA_LINK_CONTROL + NVIDIA_LINK_STRIDE);
    EXPECT_EQ(methods.methods[3].method, NVIDIA_DISPLAY_ID);
    EXPECT_EQ(methods.methods[6].method, NVIDIA_LINK_CONTROL + NVIDIA_LINK_STRIDE);
    EXPECT_EQ(methods.methods[7].method, NVIDIA_DISPLAY_ID + 2 * NVIDIA_HEAD_STRIDE);

    EXPECT_EQ(methods.methods[2].value, 0u);
    EXPECT_EQ(methods.methods[6].value, 0u);
}

// With no head active and the windows already mapped there is nothing to write
TEST(nvidia_core_methods, no_active_head_after_the_first_update_gives_no_methods) {
    shutdown_methods methods = {};
    methods.count = 1;

    build_shutdown_methods(shutdown_heads(), PRESENT_WINDOWS, active_heads{}, false, methods);

    EXPECT_EQ(methods.count, 0u);
}

// The IOC 25G2S's preferred timing, 2560 by 1440 at 60 Hz with a positive horizontal and negative vertical sync
static constexpr edid::timing IOC_25G2S_PREFERRED = {241700, 2560, 48, 32, 80, 1440, 3, 5, 33, true, false};

// Head 0 lit at `IOC_25G2S_PREFERRED` for windows 0 and 1, showing `SHOWN_DISPLAY` on link A of serial link 2
static constexpr head_light HEAD_0_LIGHT = {
    .head = 0,
    .windows = 0x3,
    .timing = IOC_25G2S_PREFERRED,
    .display_id = SHOWN_DISPLAY,
    .link = {.index = SHOWN_LINK, .sublink = 0},
};

// NVIDIA's methods and values lighting `HEAD_0_LIGHT`, in the order NVIDIA's modesetting module writes them
static constexpr display_method HEAD_0_LIT[] = {
    {0x2078, 0},          // HEAD_SET_OVERSCAN_COLOR, black
    {0x2064, 0x05C90AA0}, // HEAD_SET_RASTER_SIZE, 2720 by 1481
    {0x2068, 0x0004001F}, // HEAD_SET_RASTER_SYNC_END, 31 and 4
    {0x206C, 0x0025006F}, // HEAD_SET_RASTER_BLANK_END, 111 and 37
    {0x2070, 0x05C50A6F}, // HEAD_SET_RASTER_BLANK_START, 2671 and 1477
    {0x2218, 0x00030026}, // HEAD_SET_MIN_FRAME_IDLE, 38 lines before the visible ones and 3 after
    {0x200C, 241700000},  // HEAD_SET_PIXEL_CLOCK_FREQUENCY, in hertz
    {0x201C, 0},          // HEAD_SET_PIXEL_CLOCK_CONFIGURATION
    {0x2028, 241700000},  // HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX
    {0x207C, 0},          // HEAD_SET_FRAME_PACKED_VACTIVE_COLOR
    {0x2080, 0},          // HEAD_SET_HDMI_CTRL
    {0x22D0, 0},          // HEAD_SET_TILE_POSITION
    {0x2364, 0},          // HEAD_SET_RASTER_HBLANK_DELAY
    {0x2008, 0},          // HEAD_SET_CONTROL
    {0x0210, 0},          // SET_CONTROL, for every head
    {0x2044, 0},          // HEAD_SET_LOCK_CHAIN
    {0x2040, 0},          // HEAD_SET_LOCK_OFFSET
    {0x22D4, 0},          // HEAD_SET_DSC_CONTROL
    {0x22D8, 0},          // HEAD_SET_DSC_PPS_CONTROL
    {0x2004, 0xFC000049}, // HEAD_SET_CONTROL_OUTPUT_RESOURCE, negative vertical sync and 24 bits a pixel
    {0x2000, 0},          // HEAD_SET_PROCAMP
    {0x2238, 0x0FFF0000}, // HEAD_SET_CLAMP_RANGE_GREEN, 0 to 0xFFF
    {0x223C, 0x0FFF0000}, // HEAD_SET_CLAMP_RANGE_RED_BLUE
    {0x229C, 0},          // HEAD_SET_OCSC1CONTROL
    {0x0340, 0x00000801}, // SOR_SET_CONTROL(2), head 0 in DP_A
    {0x2020, 0x100},      // HEAD_SET_DISPLAY_ID
    {0x204C, 0x05A00A00}, // HEAD_SET_VIEWPORT_SIZE_IN, 2560 by 1440
    {0x205C, 0},          // HEAD_SET_VIEWPORT_POINT_OUT_ADJUST
    {0x2058, 0x05A00A00}, // HEAD_SET_VIEWPORT_SIZE_OUT
    {0x202C, 0x04000400}, // HEAD_SET_MAX_OUTPUT_SCALE_FACTOR, 1024 1024ths each way
    {0x1010, 0x10110A16}, // WINDOW_SET_WINDOW_USAGE_BOUNDS(0), 2560 + 22 pixels fetched a line
    {0x1090, 0x10110A16}, // WINDOW_SET_WINDOW_USAGE_BOUNDS(1)
    {0x2048, 0},          // HEAD_SET_VIEWPORT_POINT_IN
    {0x2014, 0x11},       // HEAD_SET_CONTROL_OUTPUT_SCALER, 2 taps each way
};

// Lighting `HEAD_0_LIGHT` gives NVIDIA's 34 methods and values in NVIDIA's order
TEST(nvidia_core_methods, head_light_methods_follow_nvidias_order_and_values) {
    head_light_methods methods = {};

    build_head_light_methods(HEAD_0_LIGHT, methods);

    ASSERT_EQ(methods.count, 34u);
    expect_methods(methods, 0, HEAD_0_LIT, 34);
}

// The indices in `HEAD_0_LIT` of the methods that are not a head's: the flip lock pins, the link and the windows
static constexpr uint32_t SET_CONTROL_INDEX  = 14;
static constexpr uint32_t LINK_INDEX         = 24;
static constexpr uint32_t FIRST_WINDOW_INDEX = 30;

// Head 3 lit through link B of serial link 0 for windows 6 and 7 takes each head method at head 3's, the link carrying
// head 3 in DP_B, while the flip lock pins' method stays the channel's own
TEST(nvidia_core_methods, head_light_methods_follow_the_head_link_and_windows) {
    head_light light = {
        .head = 3,
        .windows = 0xC0,
        .timing = IOC_25G2S_PREFERRED,
        .display_id = 0x800,
        .link = {.index = 0, .sublink = 1},
    };

    head_light_methods methods = {};

    build_head_light_methods(light, methods);

    ASSERT_EQ(methods.count, 34u);
    for (uint32_t index = 0; index < 34; index++) {
        bool is_flip_lock_pins = index == SET_CONTROL_INDEX;
        bool is_link = index == LINK_INDEX;
        bool is_window = index == FIRST_WINDOW_INDEX || index == FIRST_WINDOW_INDEX + 1;
        if (!is_flip_lock_pins && !is_link && !is_window) {
            EXPECT_EQ(methods.methods[index].method, HEAD_0_LIT[index].method + 3 * NVIDIA_HEAD_STRIDE);
        }
    }

    EXPECT_EQ(methods.methods[SET_CONTROL_INDEX].method, HEAD_0_LIT[SET_CONTROL_INDEX].method);
    EXPECT_EQ(methods.methods[LINK_INDEX].method, NVIDIA_LINK_CONTROL);
    EXPECT_EQ(methods.methods[LINK_INDEX].value, 0x908u);
    EXPECT_EQ(methods.methods[LINK_INDEX + 1].value, 0x800u);
    EXPECT_EQ(methods.methods[FIRST_WINDOW_INDEX].method, NVIDIA_USAGE_BOUNDS + 6 * NVIDIA_WINDOW_STRIDE);
    EXPECT_EQ(methods.methods[FIRST_WINDOW_INDEX + 1].method, NVIDIA_USAGE_BOUNDS + 7 * NVIDIA_WINDOW_STRIDE);
}

// A head serving no window gets no usage bounds, its viewport's origin following its output scale factor
TEST(nvidia_core_methods, head_light_methods_bound_no_window_for_a_head_serving_none) {
    head_light light = HEAD_0_LIGHT;
    light.windows = 0;
    head_light_methods methods = {};

    build_head_light_methods(light, methods);

    ASSERT_EQ(methods.count, 32u);
    EXPECT_EQ(methods.methods[29].method, 0x202Cu);
    EXPECT_EQ(methods.methods[30].method, 0x2048u);
    EXPECT_EQ(methods.methods[31].method, 0x2014u);
}

// A negative sync sets its polarity, bit 2 for the horizontal and bit 3 for the vertical, each alone or both
TEST(nvidia_core_methods, head_light_output_resource_takes_each_negative_sync) {
    static constexpr uint32_t OUTPUT_RESOURCE_INDEX = 19;
    head_light light = HEAD_0_LIGHT;
    head_light_methods methods = {};

    light.timing.h_sync_positive = true;
    light.timing.v_sync_positive = true;
    build_head_light_methods(light, methods);
    EXPECT_EQ(methods.methods[OUTPUT_RESOURCE_INDEX].value, 0xFC000041u);

    light.timing.h_sync_positive = false;
    build_head_light_methods(light, methods);
    EXPECT_EQ(methods.methods[OUTPUT_RESOURCE_INDEX].value, 0xFC000045u);

    light.timing.v_sync_positive = false;
    build_head_light_methods(light, methods);
    EXPECT_EQ(methods.methods[OUTPUT_RESOURCE_INDEX].value, 0xFC00004Du);

    light.timing.h_sync_positive = true;
    build_head_light_methods(light, methods);
    EXPECT_EQ(methods.methods[OUTPUT_RESOURCE_INDEX].value, 0xFC000049u);
}

// No horizontal sync or back porch and no vertical sync give 32-bit positions of 0xFFFFFFFF, which the raster's 15-bit
// fields keep as 0x7FFF in both halves, as NVIDIA's 16-bit positions of 0xFFFF become
TEST(nvidia_core_methods, head_light_raster_keeps_15_bits_of_each_position) {
    head_light light = HEAD_0_LIGHT;
    light.timing.h_sync_width = 0;
    light.timing.h_back_porch = 0;
    light.timing.v_sync_width = 0;
    light.timing.v_back_porch = 2;
    head_light_methods methods = {};

    build_head_light_methods(light, methods);

    EXPECT_EQ(methods.methods[2].value, 0x7FFF7FFFu);
    EXPECT_EQ(methods.methods[3].value, 0x00017FFFu);
}

// A pixel clock of 3,300,000 kHz, 0xC4B20100 Hz, gives both frequencies its low 31 bits, HERTZ's field, so bit 30
// stays set and bit 31, ADJ1000DIV1001, clear
TEST(nvidia_core_methods, head_light_pixel_clock_keeps_31_bits_of_its_hertz) {
    static constexpr uint32_t PIXEL_CLOCK_INDEX     = 6;
    static constexpr uint32_t PIXEL_CLOCK_MAX_INDEX = 8;
    head_light light = HEAD_0_LIGHT;
    light.timing.pixel_clock_khz = 3300000;
    head_light_methods methods = {};

    build_head_light_methods(light, methods);

    EXPECT_EQ(methods.methods[PIXEL_CLOCK_INDEX].value, 0x44B20100u);
    EXPECT_EQ(methods.methods[PIXEL_CLOCK_MAX_INDEX].value, 0x44B20100u);
}

// Window 1 serving a head whose viewport is 1920 pixels wide may scan the 4-byte formats unscaled, fetching 1920 + 22
// pixels a line
TEST(nvidia_core_methods, window_usage_methods_bound_the_formats_scale_and_fetch) {
    static constexpr display_method WINDOW_1_USAGE[] = {
        {0x1084, 0x4},        // WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(1), RGB_PACKED4BPP
        {0x108C, 0x04000400}, // WINDOW_SET_MAX_INPUT_SCALE_FACTOR(1), 1024 1024ths each way
        {0x1090, 0x10110796}, // WINDOW_SET_WINDOW_USAGE_BOUNDS(1)
    };

    window_usage_methods methods = {};

    build_window_usage_methods(1, 1920, methods);

    expect_methods(methods, 0, WINDOW_1_USAGE, 3);
}

// A viewport 49,130 pixels wide fetches 49,152 pixels a line, 0xC000, of which MAX_PIXELS_FETCHED_PER_LINE keeps its
// 15 bits, 0x4000, leaving bit 15 clear
TEST(nvidia_core_methods, window_usage_methods_keep_15_bits_of_the_pixels_fetched) {
    window_usage_methods methods = {};

    build_window_usage_methods(1, 49130, methods);

    EXPECT_EQ(methods.methods[2].value, 0x10114000u);
}
