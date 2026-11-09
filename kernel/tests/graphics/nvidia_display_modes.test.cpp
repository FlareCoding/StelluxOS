// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_modes.h"

TEST_SUITE(nvidia_display_modes);

using drivers::nvidia::find_mode_candidates;
using drivers::nvidia::LIMITED_REFRESH_BOUND_MILLIHERTZ;
using drivers::nvidia::list_modes;
using drivers::nvidia::mode_candidates;
using drivers::nvidia::mode_list;
using drivers::nvidia::mode_of;
using drivers::nvidia::displayport::link_facts;
using drivers::nvidia::displayport::link_rate;

// The AOC AG241QG4's seven timings as `edid::parse` reads its EDID, the first preferred: 2560 by 1440 at about 60, 144,
// 120 and 100 Hz, then 1920 by 1080, 1440 by 900 and 1280 by 1440 at about 60 Hz
static constexpr edid::timing AOC_AG241QG4_TIMINGS[] = {
    {241500, 2560, 48, 32, 80, 1440, 3, 5, 33, true, true},
    {592000, 2560, 8, 32, 66, 1440, 25, 8, 70, true, false},
    {487870, 2560, 8, 32, 66, 1440, 25, 8, 52, true, true},
    {402560, 2560, 8, 32, 66, 1440, 25, 8, 37, true, true},
    {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true},
    {88750, 1440, 48, 32, 80, 900, 3, 6, 17, true, false},
    {156000, 1280, 96, 136, 232, 1440, 3, 10, 40, false, true},
};

// The IOC 25G2S's six timings as `edid::parse` reads its EDID, the first preferred: 2560 by 1440 at 60 Hz, 1920 by 1080
// at 60 Hz, then 2560 by 1440 at 120, 143.999, 240 and 164.999 Hz
static constexpr edid::timing IOC_25G2S_TIMINGS[] = {
    {241700, 2560, 48, 32, 80, 1440, 3, 5, 33, true, false},
    {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true},
    {483400, 2560, 48, 32, 80, 1440, 3, 5, 33, true, false},
    {576160, 2560, 48, 32, 80, 1440, 3, 5, 23, true, false},
    {969410, 2560, 48, 32, 80, 1440, 3, 5, 37, true, false},
    {668710, 2560, 48, 32, 80, 1440, 3, 5, 42, false, false},
};

// The RTX 3080's connectors take four lanes at HBR3, the AOC AG241QG4 four at HBR2 and the IOC 25G2S four at HBR3. Both
// monitors take enhanced framing, and the GPU supports FEC and the increased watermark limits
static constexpr link_facts RTX_3080_TO_AOC_AG241QG4 = {{4, link_rate::hbr3}, {4, link_rate::hbr2}, true, true, true};
static constexpr link_facts RTX_3080_TO_IOC_25G2S    = {{4, link_rate::hbr3}, {4, link_rate::hbr3}, true, true, true};

static edid::monitor s_monitor;

static void set_timings(const edid::timing* timings, uint32_t count, bool first_is_preferred) {
    string::memset(&s_monitor, 0, sizeof(s_monitor));
    string::memcpy(s_monitor.timings, timings, count * sizeof(edid::timing));
    s_monitor.timing_count = count;
    s_monitor.first_timing_is_preferred = first_is_preferred;
}

static void expect_candidates(const mode_candidates& actual, const uint8_t* expected, uint8_t count) {
    EXPECT_EQ(actual.count, count);
    for (uint8_t rank = 0; rank < count && rank < actual.count; rank++) {
        EXPECT_EQ(actual.timings[rank], expected[rank]);
    }
}

// The preferred timing first although its 2560 by 1440 comes at the slowest refresh, then the others from the fastest
TEST(nvidia_display_modes, orders_the_ioc_25g2s_timings_from_the_preferred_one) {
    set_timings(IOC_25G2S_TIMINGS, 6, true);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, candidates);

    static constexpr uint8_t EXPECTED[] = {0, 4, 5, 3, 2, 1};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// 1280 by 1440 has more pixels than 1440 by 900, so it comes first although the EDID lists it last
TEST(nvidia_display_modes, orders_the_aoc_ag241qg4_timings_from_the_preferred_one) {
    set_timings(AOC_AG241QG4_TIMINGS, 7, true);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_AOC_AG241QG4, false, candidates);

    static constexpr uint8_t EXPECTED[] = {0, 1, 2, 3, 4, 6, 5};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

TEST(nvidia_display_modes, ranks_every_timing_when_none_is_preferred) {
    set_timings(AOC_AG241QG4_TIMINGS, 7, false);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_AOC_AG241QG4, false, candidates);

    static constexpr uint8_t EXPECTED[] = {1, 2, 3, 0, 4, 6, 5};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// The 1280 by 1440 timing left past the count would come before the 1440 by 900 one
TEST(nvidia_display_modes, leaves_out_a_timing_past_the_timing_count) {
    set_timings(AOC_AG241QG4_TIMINGS, 7, true);
    s_monitor.timing_count = 6;

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_AOC_AG241QG4, false, candidates);

    static constexpr uint8_t EXPECTED[] = {0, 1, 2, 3, 4, 5};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// 1920 by 1080 at 60 Hz, 2560 by 1440 at about 60 Hz, 1920 by 1080 at 120 Hz, then the first again with its horizontal
// sync negative, as many pixels at the same refresh
static constexpr edid::timing TIED_TIMINGS[] = {
    {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true},
    {241500, 2560, 48, 32, 80, 1440, 3, 5, 33, true, true},
    {297000, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true},
    {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, false, true},
};

TEST(nvidia_display_modes, keeps_the_edid_order_of_tied_timings) {
    set_timings(TIED_TIMINGS, 4, false);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, candidates);

    static constexpr uint8_t EXPECTED[] = {1, 2, 0, 3};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// 2560 by 1440 at about 59.96 Hz with wider blanking, so a faster pixel clock, then the IOC 25G2S's timing of that size
// at 60 Hz
static constexpr edid::timing FASTER_CLOCK_SLOWER_REFRESH_TIMINGS[] = {
    {312250, 2560, 192, 272, 464, 1440, 3, 5, 45, false, true},
    IOC_25G2S_TIMINGS[0],
};

TEST(nvidia_display_modes, ranks_timings_of_one_size_by_refresh_not_pixel_clock) {
    set_timings(FASTER_CLOCK_SLOWER_REFRESH_TIMINGS, 2, false);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, candidates);

    static constexpr uint8_t EXPECTED[] = {1, 0};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// 2560 by 1080 at 60 Hz, then 2048 by 1536 at about 60 Hz: the second has more pixels, though its longer side, the sum
// of its sides and its diagonal are each shorter
static constexpr edid::timing WIDE_THEN_TALL_TIMINGS[] = {
    {198000, 2560, 248, 44, 148, 1080, 4, 5, 11, true, true},
    {209250, 2048, 48, 32, 80, 1536, 3, 4, 37, true, false},
};

TEST(nvidia_display_modes, ranks_by_pixel_count_where_the_sides_rank_the_other_way) {
    set_timings(WIDE_THEN_TALL_TIMINGS, 2, false);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, candidates);

    static constexpr uint8_t EXPECTED[] = {1, 0};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// The IOC 25G2S's 1920 by 1080 timing marked preferred, then its 2560 by 1440 timing, both at 60 Hz
static constexpr edid::timing SMALL_PREFERRED_TIMINGS[] = {
    IOC_25G2S_TIMINGS[1],
    IOC_25G2S_TIMINGS[0],
};

// The preferred timing comes first although the link carries a timing of more pixels
TEST(nvidia_display_modes, puts_the_preferred_timing_before_a_larger_one) {
    set_timings(SMALL_PREFERRED_TIMINGS, 2, true);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, candidates);

    static constexpr uint8_t EXPECTED[] = {0, 1};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// Two lanes at HBR3 carry every timing of the AOC AG241QG4 but the 144 Hz one
TEST(nvidia_display_modes, keeps_only_the_timings_the_link_carries) {
    set_timings(AOC_AG241QG4_TIMINGS, 7, true);
    link_facts two_lanes = RTX_3080_TO_AOC_AG241QG4;
    two_lanes.monitor = {2, link_rate::hbr3};

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, two_lanes, false, candidates);

    static constexpr uint8_t EXPECTED[] = {0, 2, 3, 4, 6, 5};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// One lane at HBR2 carries no timing of 2560 by 1440, so the largest it carries comes first
TEST(nvidia_display_modes, passes_over_a_preferred_timing_the_link_cannot_carry) {
    set_timings(AOC_AG241QG4_TIMINGS, 7, true);
    link_facts one_lane = RTX_3080_TO_AOC_AG241QG4;
    one_lane.monitor = {1, link_rate::hbr2};

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, one_lane, false, candidates);

    static constexpr uint8_t EXPECTED[] = {4, 6, 5};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

TEST(nvidia_display_modes, drops_the_faster_timings_of_an_output_limited_to_60_hz) {
    set_timings(IOC_25G2S_TIMINGS, 6, true);
    mode_candidates ioc_25g2s = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, true, ioc_25g2s);

    static constexpr uint8_t IOC_25G2S_EXPECTED[] = {0, 1};
    expect_candidates(ioc_25g2s, IOC_25G2S_EXPECTED, sizeof(IOC_25G2S_EXPECTED));

    set_timings(AOC_AG241QG4_TIMINGS, 7, true);
    mode_candidates aoc_ag241qg4 = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_AOC_AG241QG4, true, aoc_ag241qg4);

    static constexpr uint8_t AOC_AG241QG4_EXPECTED[] = {0, 4, 6, 5};
    expect_candidates(aoc_ag241qg4, AOC_AG241QG4_EXPECTED, sizeof(AOC_AG241QG4_EXPECTED));
}

// The IOC 25G2S's 240 Hz timing marked preferred, then its 1920 by 1080 and 2560 by 1440 timings at 60 Hz
static constexpr edid::timing FAST_PREFERRED_TIMINGS[] = {
    IOC_25G2S_TIMINGS[4],
    IOC_25G2S_TIMINGS[1],
    IOC_25G2S_TIMINGS[0],
};

// The 60 Hz limit drops the preferred timing, so the larger timing comes first although the EDID lists it after another
TEST(nvidia_display_modes, ranks_the_rest_by_size_when_the_preferred_timing_drops) {
    set_timings(FAST_PREFERRED_TIMINGS, 3, true);

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, true, candidates);

    static constexpr uint8_t EXPECTED[] = {2, 1};
    expect_candidates(candidates, EXPECTED, sizeof(EXPECTED));
}

// Three timings of 1000 by 1000 pixels, visible or not, so each refreshes at a millihertz per kHz of its pixel clock:
// the preferred one at the bound, the next just below it, the last at 50 Hz
static constexpr edid::timing BOUND_TIMINGS[] = {
    {60600, 900, 10, 40, 50, 960, 10, 10, 20, true, true},
    {60599, 900, 10, 40, 50, 960, 10, 10, 20, true, true},
    {50000, 900, 10, 40, 50, 960, 10, 10, 20, true, true},
};

TEST(nvidia_display_modes, an_output_limited_to_60_hz_keeps_timings_below_60_6_hz) {
    set_timings(BOUND_TIMINGS, 3, true);
    EXPECT_EQ(s_monitor.timings[0].refresh_millihertz(), LIMITED_REFRESH_BOUND_MILLIHERTZ);
    EXPECT_EQ(s_monitor.timings[1].refresh_millihertz(), LIMITED_REFRESH_BOUND_MILLIHERTZ - 1);

    mode_candidates limited = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, true, limited);
    mode_candidates unlimited = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, unlimited);

    static constexpr uint8_t LIMITED_EXPECTED[] = {1, 2};
    static constexpr uint8_t UNLIMITED_EXPECTED[] = {0, 1, 2};
    expect_candidates(limited, LIMITED_EXPECTED, sizeof(LIMITED_EXPECTED));
    expect_candidates(unlimited, UNLIMITED_EXPECTED, sizeof(UNLIMITED_EXPECTED));
}

TEST(nvidia_display_modes, gives_no_timing_on_a_gpu_without_fec_support) {
    set_timings(IOC_25G2S_TIMINGS, 6, true);
    link_facts without_fec = RTX_3080_TO_IOC_25G2S;
    without_fec.fec_supported = false;

    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, without_fec, false, candidates);

    EXPECT_EQ(candidates.count, 0u);
}

// A connector taking no lanes, and an EDID giving no timing, each leave the candidates rewritten empty
TEST(nvidia_display_modes, gives_no_timing_without_lanes_or_timings) {
    set_timings(IOC_25G2S_TIMINGS, 6, true);
    link_facts without_lanes = RTX_3080_TO_IOC_25G2S;
    without_lanes.connector.max_lanes = 0;

    mode_candidates no_lanes = {.count = 3, .timings = {5, 4, 3}};
    find_mode_candidates(s_monitor, without_lanes, false, no_lanes);

    EXPECT_EQ(no_lanes.count, 0u);
    EXPECT_EQ(no_lanes.timings[0], 0u);

    set_timings(IOC_25G2S_TIMINGS, 0, false);
    mode_candidates no_timings = {.count = 3, .timings = {5, 4, 3}};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, no_timings);

    EXPECT_EQ(no_timings.count, 0u);
    EXPECT_EQ(no_timings.timings[0], 0u);
}

// Lists the modes of the timings `candidates` holds in rank order, from `s_monitor`, as the engine lists a lit output's
static void list_candidates(const mode_candidates& candidates, mode_list& out) {
    edid::timing ranked[edid::TIMINGS_MAX] = {};
    for (uint8_t rank = 0; rank < candidates.count; rank++) {
        ranked[rank] = s_monitor.timings[candidates.timings[rank]];
    }

    list_modes(ranked, candidates.count, out);
}

static void expect_mode(const gfxfb::gfxfb_mode& actual, uint32_t width, uint32_t height, uint64_t refresh_millihertz) {
    EXPECT_EQ(actual.width, width);
    EXPECT_EQ(actual.height, height);
    EXPECT_EQ(actual.refresh_millihertz, refresh_millihertz);
}

TEST(nvidia_display_modes, gives_a_timings_visible_size_and_refresh_as_its_mode) {
    gfxfb::gfxfb_mode mode = mode_of(AOC_AG241QG4_TIMINGS[1]);

    expect_mode(mode, 2560, 1440, AOC_AG241QG4_TIMINGS[1].refresh_millihertz());
    EXPECT_EQ(mode.refresh_millihertz, 143911u);
}

// The IOC 25G2S's modes, five refreshes at 2560 by 1440 and 1920 by 1080 at the first's 60 Hz, so a list telling modes
// apart by size alone or by refresh alone would drop some
TEST(nvidia_display_modes, lists_the_ioc_25g2s_modes_in_rank_order) {
    set_timings(IOC_25G2S_TIMINGS, 6, true);
    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, candidates);

    mode_list list = {};
    list_candidates(candidates, list);

    ASSERT_EQ(list.count, 6u);

    expect_mode(list.modes[0], 2560, 1440, 60000);
    expect_mode(list.modes[1], 2560, 1440, 240000);
    expect_mode(list.modes[2], 2560, 1440, 164999);
    expect_mode(list.modes[3], 2560, 1440, 143999);
    expect_mode(list.modes[4], 2560, 1440, 120000);
    expect_mode(list.modes[5], 1920, 1080, 60000);
}

// `TIED_TIMINGS`' fourth timing differs from the first in horizontal sync polarity alone, so the list drops its mode
TEST(nvidia_display_modes, lists_once_the_mode_of_timings_whose_sync_alone_differs) {
    set_timings(TIED_TIMINGS, 4, false);
    mode_candidates candidates = {};
    find_mode_candidates(s_monitor, RTX_3080_TO_IOC_25G2S, false, candidates);

    mode_list list = {};
    list_candidates(candidates, list);

    ASSERT_EQ(list.count, 3u);
    expect_mode(list.modes[0], 2560, 1440, 59950);
    expect_mode(list.modes[1], 1920, 1080, 120000);
    expect_mode(list.modes[2], 1920, 1080, 60000);
    EXPECT_EQ(list.modes[3].width, 0u);
}

TEST(nvidia_display_modes, lists_no_mode_for_no_timing) {
    mode_list list = {};
    list.count = 1;
    list.modes[0] = mode_of(IOC_25G2S_TIMINGS[0]);

    list_modes(IOC_25G2S_TIMINGS, 0, list);

    EXPECT_EQ(list.count, 0u);
    EXPECT_EQ(list.modes[0].width, 0u);
}
