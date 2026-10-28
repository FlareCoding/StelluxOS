// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_light);

using drivers::nvidia::active_heads;
using drivers::nvidia::base_buffer_pitch;
using drivers::nvidia::choose_first_light;
using drivers::nvidia::display_heads;
using drivers::nvidia::display_outputs;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::find_light_target;
using drivers::nvidia::firmware;
using drivers::nvidia::first_light;
using drivers::nvidia::head_raster;
using drivers::nvidia::light_target;
using drivers::nvidia::NO_HEAD;
using drivers::nvidia::OK;
using drivers::nvidia::output_modes;
using drivers::nvidia::raster_of;
namespace rm = drivers::nvidia::rm;

// A made-up handle for the bandwidth check's object
static constexpr rm::handle TEST_BANDWIDTH_CHECK = 0x0005000E;

// The IOC 25G2S's preferred timing, 2560 by 1440 at 60 Hz, and NVIDIA's raster for it: totals of 2720 by 1481, the
// blanking ending 32 + 80 - 1 pixels and 5 + 33 - 1 lines from the sync's start and starting 2560 and 1440 after that
static constexpr edid::timing IOC_25G2S_PREFERRED = {241700, 2560, 48, 32, 80, 1440, 3, 5, 33, true, false};
static constexpr head_raster  IOC_25G2S_RASTER = {2720, 1481, 2671, 1477, 111, 37};

// The IOC 25G2S's 1920 by 1080 timing, that timing with its vertical sync and back porch cut to 0, 1 and 2 lines, and
// with its horizontal ones cut to 0 pixels
static constexpr edid::timing IOC_25G2S_1080P = {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true};
static constexpr edid::timing NO_LEADING_LINES = {148500, 1920, 88, 44, 148, 1080, 4, 0, 0, true, true};
static constexpr edid::timing ONE_LEADING_LINE = {148500, 1920, 88, 44, 148, 1080, 4, 1, 0, true, true};
static constexpr edid::timing TWO_LEADING_LINES = {148500, 1920, 88, 44, 148, 1080, 4, 1, 1, true, true};
static constexpr edid::timing NO_LEADING_PIXELS = {148500, 1920, 88, 0, 0, 1080, 4, 5, 36, true, true};

// The parameters of the first requests the double takes, which the stack cannot hold
static constexpr uint32_t MAX_REQUESTS = 4;
static rm::mode_possible_params s_requests[MAX_REQUESTS];

// The byte the double writes over each request it takes, so a later request that is not cleared first shows it
static constexpr uint8_t REPLY_PATTERN = 0xA5;

// A firmware whose bandwidth check keeps the first requests' parameters in `s_requests`, writes `REPLY_PATTERN` over
// each request and gives it the next of `answers` in turn, accepting any request past them
class light_firmware : public firmware {
public:
    struct answer {
        int32_t result;
        bool    is_possible;
    };

    answer   answers[MAX_REQUESTS] = {};
    uint32_t answer_count = 0;
    uint32_t request_count = 0; // the bandwidth check's requests
    uint32_t other_calls = 0;   // every other call but `new_handle` and `client`

    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override {
        other_calls++;
        return OK;
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        bool is_request = object == TEST_BANDWIDTH_CHECK && command == rm::CONTROL_IS_MODE_POSSIBLE;
        if (!is_request || size != sizeof(rm::mode_possible_params)) {
            other_calls++;
            return OK;
        }

        auto* request = static_cast<rm::mode_possible_params*>(params);
        if (request_count < MAX_REQUESTS) {
            string::memcpy(&s_requests[request_count], request, sizeof(*request));
        }

        answer given = request_count < answer_count ? answers[request_count] : answer{OK, true};
        request_count++;

        string::memset(request, REPLY_PATTERN, sizeof(*request));
        request->is_possible = given.is_possible ? 1 : 0;
        return given.result;
    }

    int32_t release(rm::handle) override {
        other_calls++;
        return OK;
    }

    int32_t internal_control(uint32_t, void*, size_t) override {
        other_calls++;
        return OK;
    }

    rm::handle new_handle() override {
        return 0;
    }

    rm::handle client() const override {
        return 0;
    }
};

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
static int32_t choose(
    light_firmware& fw,
    uint8_t output_count,
    uint8_t first_output,
    first_light& light,
    bool& is_chosen
) {
    light_target target = {.head = 1, .window = 2, .first_output = first_output};
    return choose_first_light(fw, TEST_BANDWIDTH_CHECK, s_modes, output_count, target, light, is_chosen);
}

// Whether each of the `size` bytes at `bytes` is 0
static bool is_zero(const void* bytes, size_t size) {
    const auto* byte = static_cast<const uint8_t*>(bytes);
    for (size_t index = 0; index < size; index++) {
        if (byte[index] != 0) {
            return false;
        }
    }

    return true;
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

TEST(nvidia_display_light, raster_of_follows_nvidias_derivation) {
    head_raster raster = raster_of(IOC_25G2S_PREFERRED);

    EXPECT_EQ(raster.width, IOC_25G2S_RASTER.width);
    EXPECT_EQ(raster.height, IOC_25G2S_RASTER.height);
    EXPECT_EQ(raster.blank_start_x, IOC_25G2S_RASTER.blank_start_x);
    EXPECT_EQ(raster.blank_start_y, IOC_25G2S_RASTER.blank_start_y);
    EXPECT_EQ(raster.blank_end_x, IOC_25G2S_RASTER.blank_end_x);
    EXPECT_EQ(raster.blank_end_y, IOC_25G2S_RASTER.blank_end_y);
}

// A line with no sync or back porch pixel ends its blanking at 0xFFFFFFFF, where NVIDIA's 16-bit positions give 0xFFFF
TEST(nvidia_display_light, raster_of_gives_a_32_bit_blanking_end_without_horizontal_sync_or_back_porch) {
    head_raster raster = raster_of(NO_LEADING_PIXELS);

    EXPECT_EQ(raster.blank_end_x, 0xFFFFFFFFu);
    EXPECT_EQ(raster.blank_start_x, NO_LEADING_PIXELS.width - 1);
}

// 1366 pixels take 5464 bytes, which 256-byte rounding makes 5632 and 64-byte rounding would make 5504, and 65,536
// pixels, the widest an EDID timing gives and too wide for 16 bits, take 262,144 bytes
TEST(nvidia_display_light, base_buffer_pitch_rounds_rows_of_4_byte_pixels_up_to_256_bytes) {
    EXPECT_EQ(base_buffer_pitch(1366), 5632u);
    EXPECT_EQ(base_buffer_pitch(2560), 10240u);
    EXPECT_EQ(base_buffer_pitch(64), 256u);
    EXPECT_EQ(base_buffer_pitch(65), 512u);
    EXPECT_EQ(base_buffer_pitch(1), 256u);
    EXPECT_EQ(base_buffer_pitch(65536), 262144u);
}

// The request NVIDIA's modesetting module would send for this head and window alone, unscaled and 4 bytes a pixel
TEST(nvidia_display_light, fills_the_one_head_and_window_as_nvidia_would) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);

    light_firmware fw;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 1, 0, light, is_chosen), OK);

    ASSERT_EQ(fw.request_count, 1u);
    EXPECT_EQ(fw.other_calls, 0u);

    const rm::mode_possible_params& request = s_requests[0];
    EXPECT_EQ(request.subdevice_index, 0u);
    EXPECT_EQ(request.head_count, 1u);
    EXPECT_EQ(request.window_count, 1u);
    EXPECT_TRUE(is_zero(&request.heads[1], sizeof(request.heads) - sizeof(request.heads[0])));
    EXPECT_TRUE(is_zero(&request.windows[1], sizeof(request.windows) - sizeof(request.windows[0])));

    EXPECT_EQ(request.options, 0u);
    EXPECT_EQ(request.test_memory_clock_khz, 0u);
    EXPECT_EQ(request.uses_cached_perf_state, 0u);

    const rm::mode_possible_head& head = request.heads[0];
    EXPECT_EQ(head.head, 1u);
    EXPECT_EQ(head.pixel_clock_khz, 241700u);
    EXPECT_EQ(head.raster_width, IOC_25G2S_RASTER.width);
    EXPECT_EQ(head.raster_height, IOC_25G2S_RASTER.height);

    EXPECT_EQ(head.blank_start_x, IOC_25G2S_RASTER.blank_start_x);
    EXPECT_EQ(head.blank_start_y, IOC_25G2S_RASTER.blank_start_y);
    EXPECT_EQ(head.blank_end_x, IOC_25G2S_RASTER.blank_end_x);
    EXPECT_EQ(head.blank_end_y, IOC_25G2S_RASTER.blank_end_y);
    EXPECT_EQ(head.second_blank_start_y, 0u);
    EXPECT_EQ(head.second_blank_end_y, 0u);

    EXPECT_EQ(head.master_lock_mode, 0x0u);
    EXPECT_EQ(head.master_lock_pin, 0x10u);
    EXPECT_EQ(head.slave_lock_mode, 0x0u);
    EXPECT_EQ(head.slave_lock_pin, 0x10u);

    EXPECT_EQ(head.max_downscale_x, 1024u);
    EXPECT_EQ(head.max_downscale_y, 1024u);
    EXPECT_EQ(head.output_scaler_taps, 2u);
    EXPECT_EQ(head.allows_upscaling_y, 0u);
    EXPECT_EQ(head.enables_overfetch, 0u);

    EXPECT_EQ(head.leading_idle_lines, 5u + 33u);
    EXPECT_EQ(head.trailing_idle_lines, 3u);
    EXPECT_EQ(head.lut, 2u);
    EXPECT_EQ(head.cursor_width_32, 256u / 32u);

    EXPECT_EQ(head.enables_dsc, 0u);
    EXPECT_EQ(head.is_yuv420, 0u);
    EXPECT_EQ(head.is_two_heads_one_or, 0u);
    EXPECT_EQ(head.gets_osld_output, 0u);
    EXPECT_EQ(head.disables_mid_frame_watermark, 0u);

    const rm::mode_possible_window& window = request.windows[0];
    EXPECT_EQ(window.window, 2u);
    EXPECT_EQ(window.head, 1u);
    EXPECT_EQ(window.formats, 0x4u);
    EXPECT_EQ(window.rotated_formats, 0u);

    EXPECT_EQ(window.max_pixels_fetched, (((2560u + 14u) * 1024u + 1023u) >> 10) + 8u);
    EXPECT_EQ(window.max_downscale_x, 1024u);
    EXPECT_EQ(window.max_downscale_y, 1024u);
    EXPECT_EQ(window.input_scaler_taps, 2u);

    EXPECT_EQ(window.allows_upscaling_y, 0u);
    EXPECT_EQ(window.enables_overfetch, 0u);
    EXPECT_EQ(window.lut, 2u);
    EXPECT_EQ(window.tmo_lut, 2u);

    ASSERT_TRUE(is_chosen);
    EXPECT_EQ(light.output, 0u);
    EXPECT_EQ(light.timing_index, 0u);
    EXPECT_TRUE(light.timing == IOC_25G2S_PREFERRED);
    EXPECT_EQ(light.head, 1u);
    EXPECT_EQ(light.window, 2u);
}

TEST(nvidia_display_light, takes_the_output_s_first_mode_on_the_head_and_window) {
    clear_modes();
    add_mode(0, 3, IOC_25G2S_PREFERRED);
    add_mode(0, 1, IOC_25G2S_1080P);

    light_firmware fw;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 1, 0, light, is_chosen), OK);
    ASSERT_TRUE(is_chosen);

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

    light_firmware fw;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 3, 2, light, is_chosen), OK);
    ASSERT_TRUE(is_chosen);

    EXPECT_EQ(fw.request_count, 1u);
    EXPECT_EQ(light.output, 2u);
    EXPECT_EQ(light.timing_index, 1u);
}

// Output 1 tried first, then 0 and 2 in ascending order, outputs 1 and 0 having no mode
TEST(nvidia_display_light, goes_on_in_ascending_order_past_outputs_without_a_mode) {
    clear_modes();
    add_mode(2, 1, IOC_25G2S_1080P);

    light_firmware fw;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 3, 1, light, is_chosen), OK);
    ASSERT_TRUE(is_chosen);

    EXPECT_EQ(fw.request_count, 1u);
    EXPECT_EQ(light.output, 2u);
    EXPECT_EQ(light.timing_index, 1u);
}

// Output 1 tried first, then the others from output 0 up
TEST(nvidia_display_light, takes_the_lowest_output_with_a_mode_after_the_first_one) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);
    add_mode(2, 1, IOC_25G2S_1080P);

    light_firmware fw;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 3, 1, light, is_chosen), OK);
    ASSERT_TRUE(is_chosen);

    EXPECT_EQ(light.output, 0u);
}

TEST(nvidia_display_light, chooses_nothing_when_no_output_has_a_mode) {
    clear_modes();

    light_firmware fw;
    first_light light = {};
    light.output = 7;
    bool is_chosen = true;
    ASSERT_EQ(choose(fw, 3, 0, light, is_chosen), OK);

    EXPECT_FALSE(is_chosen);
    EXPECT_EQ(fw.request_count, 0u);
    EXPECT_EQ(light.output, 7u);
}

// Output 1 tried first, then 0 and 2 in ascending order, output 0 having no mode to ask about
TEST(nvidia_display_light, goes_on_to_the_others_in_ascending_order_after_a_refused_output) {
    clear_modes();
    add_mode(1, 4, IOC_25G2S_PREFERRED);
    add_mode(2, 1, IOC_25G2S_1080P);

    light_firmware fw;
    fw.answers[0] = {OK, false};
    fw.answer_count = 1;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 3, 1, light, is_chosen), OK);

    ASSERT_EQ(fw.request_count, 2u);
    EXPECT_EQ(s_requests[0].heads[0].pixel_clock_khz, IOC_25G2S_PREFERRED.pixel_clock_khz);
    EXPECT_EQ(s_requests[1].heads[0].pixel_clock_khz, IOC_25G2S_1080P.pixel_clock_khz);
    ASSERT_TRUE(is_chosen);
    EXPECT_EQ(light.output, 2u);
    EXPECT_EQ(light.timing_index, 1u);
}

TEST(nvidia_display_light, goes_on_to_the_output_s_next_mode_after_a_refused_one) {
    clear_modes();
    add_mode(0, 3, IOC_25G2S_PREFERRED);
    add_mode(0, 1, IOC_25G2S_1080P);
    add_mode(1, 0, IOC_25G2S_PREFERRED);

    light_firmware fw;
    fw.answers[0] = {OK, false};
    fw.answer_count = 1;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 2, 0, light, is_chosen), OK);

    EXPECT_EQ(fw.request_count, 2u);
    ASSERT_TRUE(is_chosen);
    EXPECT_EQ(light.output, 0u);
    EXPECT_EQ(light.timing_index, 1u);
}

// The second request of `refills_the_whole_request_for_each_mode`, kept while a fresh choice sends its first
static rm::mode_possible_params s_second_request;

// The reply overwrites the request, so the request for the next mode must equal the one a fresh choice sends first
TEST(nvidia_display_light, refills_the_whole_request_for_each_mode) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);
    add_mode(0, 1, IOC_25G2S_1080P);

    light_firmware fw;
    fw.answers[0] = {OK, false};
    fw.answer_count = 1;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 1, 0, light, is_chosen), OK);

    ASSERT_EQ(fw.request_count, 2u);
    string::memcpy(&s_second_request, &s_requests[1], sizeof(s_second_request));

    clear_modes();
    add_mode(0, 1, IOC_25G2S_1080P);
    light_firmware fresh_fw;
    ASSERT_EQ(choose(fresh_fw, 1, 0, light, is_chosen), OK);

    ASSERT_EQ(fresh_fw.request_count, 1u);
    EXPECT_EQ(string::memcmp(&s_second_request, &s_requests[0], sizeof(s_second_request)), 0);
}

// NVIDIA's modesetting module takes any failed request as a refused mode, where only a request the firmware refuses
// refuses its mode here and any other failure ends the choice
TEST(nvidia_display_light, request_the_firmware_refuses_refuses_the_mode) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);
    add_mode(0, 1, IOC_25G2S_1080P);

    light_firmware fw;
    fw.answers[0] = {ERR_FIRMWARE, true};
    fw.answer_count = 1;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 1, 0, light, is_chosen), OK);

    EXPECT_EQ(fw.request_count, 2u);
    ASSERT_TRUE(is_chosen);
    EXPECT_EQ(light.timing_index, 1u);
}

// A request timing out ends the choice and leaves `out` unchanged, where NVIDIA's modesetting module refuses the mode
TEST(nvidia_display_light, request_failing_otherwise_ends_the_choice) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);
    add_mode(1, 0, IOC_25G2S_1080P);

    light_firmware fw;
    fw.answers[0] = {ERR_TIMEOUT, true};
    fw.answer_count = 1;

    first_light light = {};
    light.output = 7;
    bool is_chosen = true;
    EXPECT_EQ(choose(fw, 2, 0, light, is_chosen), ERR_TIMEOUT);

    EXPECT_EQ(fw.request_count, 1u);
    EXPECT_TRUE(is_chosen);
    EXPECT_EQ(light.output, 7u);
}

// A mode whose sync and back porch take fewer than 2 lines is refused without asking the bandwidth check, as NVIDIA's
// modesetting module refuses a mode of 1 line but asks about one of 0, and a mode of 2 lines is asked
TEST(nvidia_display_light, modes_with_fewer_than_2_lines_of_sync_and_back_porch_are_refused_unasked) {
    clear_modes();
    add_mode(0, 0, NO_LEADING_LINES);
    add_mode(0, 1, ONE_LEADING_LINE);
    add_mode(0, 2, TWO_LEADING_LINES);

    light_firmware fw;
    first_light light = {};
    bool is_chosen = false;
    ASSERT_EQ(choose(fw, 1, 0, light, is_chosen), OK);

    ASSERT_EQ(fw.request_count, 1u);
    EXPECT_EQ(s_requests[0].heads[0].leading_idle_lines, 2u);
    EXPECT_EQ(s_requests[0].heads[0].trailing_idle_lines, 4u);
    ASSERT_TRUE(is_chosen);
    EXPECT_EQ(light.timing_index, 2u);
}

TEST(nvidia_display_light, every_mode_refused_chooses_nothing) {
    clear_modes();
    add_mode(0, 0, IOC_25G2S_PREFERRED);
    add_mode(1, 0, IOC_25G2S_1080P);

    light_firmware fw;
    fw.answers[0] = {OK, false};
    fw.answers[1] = {ERR_FIRMWARE, false};
    fw.answer_count = 2;

    first_light light = {};
    light.output = 7;
    bool is_chosen = true;
    ASSERT_EQ(choose(fw, 2, 0, light, is_chosen), OK);

    EXPECT_EQ(fw.request_count, 2u);
    EXPECT_FALSE(is_chosen);
    EXPECT_EQ(light.output, 7u);
}
