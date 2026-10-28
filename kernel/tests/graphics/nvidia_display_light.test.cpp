#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_light.h"

TEST_SUITE(nvidia_display_light);

using drivers::nvidia::active_heads;
using drivers::nvidia::choose_first_light;
using drivers::nvidia::display_heads;
using drivers::nvidia::display_outputs;
using drivers::nvidia::find_light_target;
using drivers::nvidia::first_light;
using drivers::nvidia::light_target;
using drivers::nvidia::NO_HEAD;
using drivers::nvidia::output_modes;

// The IOC 25G2S's 2560 by 1440 and 1920 by 1080 timings at 60 Hz
static constexpr edid::timing IOC_25G2S_PREFERRED = {241700, 2560, 48, 32, 80, 1440, 3, 5, 33, true, false};
static constexpr edid::timing IOC_25G2S_1080P = {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true};

// Each test's heads, the display ids they show and the outputs, for `find_light_target`
static display_heads   s_heads;
static active_heads    s_active;
static display_outputs s_outputs;

// Each test's outputs' modes, built by `add_mode`
static output_modes s_modes[3];

// Sets `head_count` heads with no window serving one, no head showing a display id and `output_count` outputs, output N
// with display id 0x100 << N
static void clear_target_facts(uint8_t head_count, uint8_t output_count) {
    s_heads.count = head_count;
    string::memset(s_heads.window_head, NO_HEAD, sizeof(s_heads.window_head));

    string::memset(&s_active, 0, sizeof(s_active));

    string::memset(&s_outputs, 0, sizeof(s_outputs));
    s_outputs.count = output_count;
    for (uint8_t index = 0; index < output_count; index++) {
        s_outputs.outputs[index].display_id = 0x100u << index;
    }
}

// Has windows 2N and 2N + 1 serve head N for each head, as in a flexible mapping
static void serve_heads_flexibly() {
    for (uint8_t window = 0; window < s_heads.count * 2; window++) {
        s_heads.window_head[window] = window / 2;
    }
}

// Appends `timing`, the EDID's timing `index`, to the modes of output `output`
static void add_mode(uint8_t output, uint8_t index, const edid::timing& timing) {
    output_modes& modes = s_modes[output];
    modes.indices[modes.count] = index;
    modes.timings[modes.count] = timing;
    modes.count++;
}

// Empties every output's modes
static void clear_modes() {
    string::memset(s_modes, 0, sizeof(s_modes));
}

// Chooses among the first `output_count` outputs from `first_output`, on head 1 with window 2
static bool choose(uint8_t output_count, uint8_t first_output, first_light& light) {
    light_target target = {.head = 1, .window = 2, .first_output = first_output};
    return choose_first_light(s_modes, output_count, target, light);
}

// Head 1, which windows 2 and 3 serve, shows output 2's display id
TEST(nvidia_display_light, targets_the_boot_head_with_its_lowest_window_and_its_output) {
    clear_target_facts(2, 3);
    serve_heads_flexibly();
    s_active.heads[1].display_id = 0x400;

    light_target target = find_light_target(s_heads, s_active, s_outputs);

    EXPECT_EQ(target.head, 1u);
    EXPECT_EQ(target.window, 2u);
    EXPECT_EQ(target.first_output, 2u);
}

// Head 1 shows display id 0x1000, which names no output
TEST(nvidia_display_light, targets_output_0_when_the_boot_head_shows_no_output) {
    clear_target_facts(2, 3);
    serve_heads_flexibly();
    s_active.heads[1].display_id = 0x1000;

    light_target target = find_light_target(s_heads, s_active, s_outputs);

    EXPECT_EQ(target.head, 1u);
    EXPECT_EQ(target.window, 2u);
    EXPECT_EQ(target.first_output, 0u);
}

TEST(nvidia_display_light, targets_head_0_and_output_0_when_no_head_is_active) {
    clear_target_facts(2, 3);
    serve_heads_flexibly();

    light_target target = find_light_target(s_heads, s_active, s_outputs);

    EXPECT_EQ(target.head, 0u);
    EXPECT_EQ(target.window, 0u);
    EXPECT_EQ(target.first_output, 0u);
}

// Windows 3 and 5 serve head 1 and window 4 head 0, so head 1's lowest window is neither 2 nor 1
TEST(nvidia_display_light, targets_the_lowest_window_of_the_head_outside_a_flexible_mapping) {
    clear_target_facts(2, 3);
    s_heads.window_head[3] = 1;
    s_heads.window_head[4] = 0;
    s_heads.window_head[5] = 1;
    s_active.heads[1].display_id = 0x200;

    light_target target = find_light_target(s_heads, s_active, s_outputs);

    EXPECT_EQ(target.head, 1u);
    EXPECT_EQ(target.window, 3u);
    EXPECT_EQ(target.first_output, 1u);
}

TEST(nvidia_display_light, takes_the_output_s_first_mode_on_the_head_and_window) {
    clear_modes();
    add_mode(0, 3, IOC_25G2S_PREFERRED);
    add_mode(0, 1, IOC_25G2S_1080P);

    first_light light = {};
    ASSERT_TRUE(choose(1, 0, light));

    EXPECT_EQ(light.output, 0u);
    EXPECT_EQ(light.timing_index, 3u);
    EXPECT_TRUE(light.timing == IOC_25G2S_PREFERRED);
    EXPECT_EQ(light.head, 1u);
    EXPECT_EQ(light.window, 2u);
}

TEST(nvidia_display_light, tries_the_first_output_before_lower_ones) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);
    add_mode(1, 0, IOC_25G2S_PREFERRED);
    add_mode(2, 1, IOC_25G2S_1080P);

    first_light light = {};
    ASSERT_TRUE(choose(3, 2, light));

    EXPECT_EQ(light.output, 2u);
    EXPECT_EQ(light.timing_index, 1u);
}

// Output 1 tried first, then 0 and 2 in ascending order, outputs 1 and 0 having no mode
TEST(nvidia_display_light, goes_on_in_ascending_order_past_outputs_without_a_mode) {
    clear_modes();
    add_mode(2, 1, IOC_25G2S_1080P);

    first_light light = {};
    ASSERT_TRUE(choose(3, 1, light));

    EXPECT_EQ(light.output, 2u);
    EXPECT_EQ(light.timing_index, 1u);
}

// Output 1 tried first, then the others from output 0 up
TEST(nvidia_display_light, takes_the_lowest_output_with_a_mode_after_the_first_one) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);
    add_mode(2, 1, IOC_25G2S_1080P);

    first_light light = {};
    ASSERT_TRUE(choose(3, 1, light));

    EXPECT_EQ(light.output, 0u);
}

TEST(nvidia_display_light, chooses_nothing_when_no_output_has_a_mode) {
    clear_modes();

    first_light light = {};
    light.output = 7;
    EXPECT_FALSE(choose(3, 0, light));

    EXPECT_EQ(light.output, 7u);
}
