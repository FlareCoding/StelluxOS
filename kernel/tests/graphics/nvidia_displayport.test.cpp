// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/displayport.h"

TEST_SUITE(nvidia_displayport);

using drivers::nvidia::displayport::compute_stream;
using drivers::nvidia::displayport::find_lowest_configuration;
using drivers::nvidia::displayport::is_in_sync;
using drivers::nvidia::displayport::is_trained;
using drivers::nvidia::displayport::link_configuration;
using drivers::nvidia::displayport::link_facts;
using drivers::nvidia::displayport::link_limits;
using drivers::nvidia::displayport::link_rate;
using drivers::nvidia::displayport::link_status;
using drivers::nvidia::displayport::stream_parameters;

// The monitors' timings as `edid::parse` reads their EDIDs, and CTA-861's 1080p at 60 Hz
static constexpr edid::timing AOC_AG241QG4_60_HZ  = {241500, 2560, 48, 32, 80, 1440, 3, 5, 33, true, true};
static constexpr edid::timing AOC_AG241QG4_144_HZ = {592000, 2560, 8, 32, 66, 1440, 25, 8, 70, true, false};
static constexpr edid::timing IOC_25G2S_240_HZ    = {969410, 2560, 48, 32, 80, 1440, 3, 5, 37, true, false};
static constexpr edid::timing FULL_HD_60_HZ       = {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true};

// The RTX 3080's DisplayPort connectors as its firmware reports them, and the two monitors as their DPCDs do
static constexpr link_limits RTX_3080_CONNECTOR = {4, link_rate::hbr3};
static constexpr link_limits AOC_AG241QG4_DPCD  = {4, link_rate::hbr2};
static constexpr link_limits IOC_25G2S_DPCD     = {4, link_rate::hbr3};

// Both monitors take enhanced framing, and the RTX 3080's firmware reports FEC support and increased watermark limits
static constexpr link_facts RTX_3080_TO_AOC_AG241QG4 = {RTX_3080_CONNECTOR, AOC_AG241QG4_DPCD, true, true, true};
static constexpr link_facts RTX_3080_TO_IOC_25G2S    = {RTX_3080_CONNECTOR, IOC_25G2S_DPCD, true, true, true};

static void expect_configuration(const link_configuration& actual, uint8_t lanes, link_rate rate) {
    EXPECT_EQ(actual.lanes, lanes);
    EXPECT_EQ(actual.rate, rate);
}

static void expect_stream(const stream_parameters& actual, const stream_parameters& expected) {
    EXPECT_EQ(actual.transfer_unit_symbols, expected.transfer_unit_symbols);
    EXPECT_EQ(actual.watermark, expected.watermark);
    EXPECT_EQ(actual.hblank_symbols, expected.hblank_symbols);
    EXPECT_EQ(actual.vblank_symbols, expected.vblank_symbols);
}

// The configuration and parameters NVIDIA's driver lit the monitor with on the RTX 3080 at 144 Hz
TEST(nvidia_displayport, carries_the_aoc_ag241qg4_at_144_hz_on_4_lanes_at_hbr2) {
    link_configuration config = {};
    ASSERT_TRUE(find_lowest_configuration(RTX_3080_TO_AOC_AG241QG4, AOC_AG241QG4_144_HZ, config));
    expect_configuration(config, 4, link_rate::hbr2);

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, config, AOC_AG241QG4_144_HZ, stream));
    expect_stream(stream, {64, 24, 0x4E, 0x8FB});
}

// The configuration and parameters NVIDIA's driver lit the monitor with on the RTX 3080 at 240 Hz: 4 lanes at HBR3,
// the last in NVIDIA's order, with the watermark raised to the increased limits' minimum
TEST(nvidia_displayport, carries_the_ioc_25g2s_at_240_hz_on_4_lanes_at_hbr3) {
    link_configuration config = {};
    ASSERT_TRUE(find_lowest_configuration(RTX_3080_TO_IOC_25G2S, IOC_25G2S_240_HZ, config));
    expect_configuration(config, 4, link_rate::hbr3);

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_IOC_25G2S, config, IOC_25G2S_240_HZ, stream));
    expect_stream(stream, {64, 22, 0x73, 0x849});
}

// The configuration NVIDIA's driver trains for this timing: 2 lanes at HBR2 and 4 lanes at HBR give the same product,
// so the fewer lanes come first
TEST(nvidia_displayport, carries_the_aoc_ag241qg4_at_60_hz_on_2_lanes_at_hbr2) {
    link_configuration config = {};
    ASSERT_TRUE(find_lowest_configuration(RTX_3080_TO_AOC_AG241QG4, AOC_AG241QG4_60_HZ, config));
    expect_configuration(config, 2, link_rate::hbr2);

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, config, AOC_AG241QG4_60_HZ, stream));
    expect_stream(stream, {64, 33, 0x14C, 0x163D});
}

// 1 lane at HBR2 and 2 lanes at HBR give the same product, so 1 lane at HBR2 comes first, and 2 lanes at HBR when the
// monitor's highest rate is HBR
TEST(nvidia_displayport, carries_full_hd_on_1_lane_at_hbr2_or_2_lanes_at_hbr_for_an_hbr_monitor) {
    link_configuration config = {};
    ASSERT_TRUE(find_lowest_configuration(RTX_3080_TO_AOC_AG241QG4, FULL_HD_60_HZ, config));
    expect_configuration(config, 1, link_rate::hbr2);

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, config, FULL_HD_60_HZ, stream));
    expect_stream(stream, {64, 33, 982, 6928});

    link_facts hbr_monitor = RTX_3080_TO_AOC_AG241QG4;
    hbr_monitor.monitor.max_rate = link_rate::hbr;
    ASSERT_TRUE(find_lowest_configuration(hbr_monitor, FULL_HD_60_HZ, config));
    expect_configuration(config, 2, link_rate::hbr);
}

struct displayport_order_case {
    uint32_t           pixel_clock_khz;
    link_configuration lowest;
};

// With both ends taking 4 lanes and HBR3, each pixel clock needs more bits per second than every configuration before
// its expected one moves
static constexpr displayport_order_case ORDER_CASES[] = {
    {50000, {1, link_rate::rbr}},   {80000, {1, link_rate::hbr}},   {100000, {2, link_rate::rbr}},
    {150000, {1, link_rate::hbr2}}, {200000, {4, link_rate::rbr}},  {250000, {1, link_rate::hbr3}},
    {300000, {2, link_rate::hbr2}}, {500000, {2, link_rate::hbr3}}, {600000, {4, link_rate::hbr2}},
    {1000000, {4, link_rate::hbr3}},
};

TEST(nvidia_displayport, tries_configurations_in_nvidias_order) {
    for (const displayport_order_case& order_case : ORDER_CASES) {
        edid::timing timing = AOC_AG241QG4_60_HZ;
        timing.pixel_clock_khz = order_case.pixel_clock_khz;

        link_configuration config = {};
        ASSERT_TRUE(find_lowest_configuration(RTX_3080_TO_IOC_25G2S, timing, config));
        expect_configuration(config, order_case.lowest.lanes, order_case.lowest.rate);
    }
}

// When both ends take HBR3, 1 lane at HBR3, a lower product than 2 lanes at HBR2, carries the AOC AG241QG4's 60 Hz
// timing
TEST(nvidia_displayport, finds_configurations_within_both_ends_rates) {
    link_configuration config = {};
    ASSERT_TRUE(find_lowest_configuration(RTX_3080_TO_IOC_25G2S, AOC_AG241QG4_60_HZ, config));
    expect_configuration(config, 1, link_rate::hbr3);

    link_facts hbr2_connector = RTX_3080_TO_IOC_25G2S;
    hbr2_connector.connector.max_rate = link_rate::hbr2;
    ASSERT_TRUE(find_lowest_configuration(hbr2_connector, AOC_AG241QG4_60_HZ, config));
    expect_configuration(config, 2, link_rate::hbr2);

    link_facts hbr_monitor = RTX_3080_TO_IOC_25G2S;
    hbr_monitor.monitor.max_rate = link_rate::hbr;
    ASSERT_TRUE(find_lowest_configuration(hbr_monitor, AOC_AG241QG4_60_HZ, config));
    expect_configuration(config, 4, link_rate::hbr);
}

// Only 4 lanes at HBR3 carry the IOC 25G2S at 240 Hz
TEST(nvidia_displayport, finds_configurations_within_both_ends_lanes) {
    link_facts two_lane_connector = RTX_3080_TO_IOC_25G2S;
    two_lane_connector.connector.max_lanes = 2;
    link_facts two_lane_monitor = RTX_3080_TO_IOC_25G2S;
    two_lane_monitor.monitor.max_lanes = 2;

    link_configuration config = {3, link_rate::hbr};
    EXPECT_FALSE(find_lowest_configuration(two_lane_connector, IOC_25G2S_240_HZ, config));
    EXPECT_FALSE(find_lowest_configuration(two_lane_monitor, IOC_25G2S_240_HZ, config));
    expect_configuration(config, 3, link_rate::hbr);

    ASSERT_TRUE(find_lowest_configuration(two_lane_monitor, AOC_AG241QG4_60_HZ, config));
    expect_configuration(config, 1, link_rate::hbr3);

    two_lane_monitor.monitor.max_rate = link_rate::hbr2;
    ASSERT_TRUE(find_lowest_configuration(two_lane_monitor, AOC_AG241QG4_60_HZ, config));
    expect_configuration(config, 2, link_rate::hbr2);
}

// Limits holding other values than 1, 2 or 4 lanes and the four rates, 0 included. As bounds, 3 and 8 lanes and the
// rate code 0x19 would each leave 2 lanes at HBR2, which carries the AOC AG241QG4's 60 Hz timing
static constexpr link_limits LIMITS_NOT_TAKEN[] = {
    {0, link_rate::hbr2},
    {3, link_rate::hbr2},
    {8, link_rate::hbr2},
    {4, static_cast<link_rate>(0)},
    {4, static_cast<link_rate>(0x19)},
};

TEST(nvidia_displayport, finds_no_configuration_for_limits_outside_the_lane_counts_and_rates) {
    for (const link_limits& limits : LIMITS_NOT_TAKEN) {
        link_facts at_connector = RTX_3080_TO_AOC_AG241QG4;
        at_connector.connector = limits;
        link_facts at_monitor = RTX_3080_TO_AOC_AG241QG4;
        at_monitor.monitor = limits;

        link_configuration config = {3, link_rate::hbr};
        EXPECT_FALSE(find_lowest_configuration(at_connector, AOC_AG241QG4_60_HZ, config));
        EXPECT_FALSE(find_lowest_configuration(at_monitor, AOC_AG241QG4_60_HZ, config));
        expect_configuration(config, 3, link_rate::hbr);
    }
}

// A lane at RBR less 0.5% moves 1289.52 million bits per second, which 24-bit pixels fill exactly at 53.73 MHz, so
// that clock is refused and 1 kHz below it carried
TEST(nvidia_displayport, refuses_a_pixel_clock_its_lanes_cannot_carry) {
    edid::timing carried = {53729, 640, 16, 96, 48, 480, 10, 2, 33, false, false};
    edid::timing too_fast = carried;
    too_fast.pixel_clock_khz = 53730;
    link_configuration one_rbr_lane = {1, link_rate::rbr};

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, one_rbr_lane, carried, stream));
    expect_stream(stream, {64, 24, 448, 1878});

    stream = {1, 2, 3, 4};
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, one_rbr_lane, too_fast, stream));
    expect_stream(stream, {1, 2, 3, 4});
}

// At this clock 4 lanes at HBR2 need 12 pixels of horizontal blanking with enhanced framing and 9 without, at their
// full rate rather than less 0.5%
TEST(nvidia_displayport, refuses_a_horizontal_blanking_shorter_than_the_symbols_around_a_line) {
    edid::timing shortest = {606150, 2560, 4, 4, 4, 1440, 25, 8, 70, true, false};
    edid::timing one_short = shortest;
    one_short.h_back_porch = 3;
    link_configuration four_hbr2_lanes = {4, link_rate::hbr2};
    link_facts no_enhanced_framing = RTX_3080_TO_AOC_AG241QG4;
    no_enhanced_framing.enhanced_framing = false;

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, four_hbr2_lanes, shortest, stream));
    expect_stream(stream, {64, 23, 0, 2159});

    stream = {1, 2, 3, 4};
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, four_hbr2_lanes, one_short, stream));
    expect_stream(stream, {1, 2, 3, 4});

    // Its horizontal blanking symbols are -6 before their clamp, against the first case's -7, so a line of vertical
    // blanking leaves 1 symbol more
    ASSERT_TRUE(compute_stream(no_enhanced_framing, four_hbr2_lanes, one_short, stream));
    expect_stream(stream, {64, 23, 0, 2160});
}

TEST(nvidia_displayport, refuses_a_timing_60_pixels_wide_or_narrower) {
    edid::timing narrowest = {10000, 61, 20, 20, 99, 100, 1, 1, 1, true, true};
    edid::timing too_narrow = {10000, 60, 20, 20, 100, 100, 1, 1, 1, true, true};
    link_configuration one_rbr_lane = {1, link_rate::rbr};

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, one_rbr_lane, narrowest, stream));
    expect_stream(stream, {64, 37, 2163, 897});

    stream = {1, 2, 3, 4};
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, one_rbr_lane, too_narrow, stream));
    expect_stream(stream, {1, 2, 3, 4});
}

// 61 pixels at 200 MHz on 4 lanes at RBR leave a line of vertical blanking 5 symbols short of what it must hold
TEST(nvidia_displayport, leaves_0_vertical_blanking_symbols_when_a_line_cannot_hold_its_packets) {
    edid::timing narrow_and_fast = {200000, 61, 20, 20, 60, 100, 1, 1, 1, true, true};
    link_configuration four_rbr_lanes = {4, link_rate::rbr};

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, four_rbr_lanes, narrow_and_fast, stream));
    expect_stream(stream, {64, 22, 60, 0});
}

// 1366 pixels make 341.5 per lane on 4 lanes, padded to 342, which adds 2 symbols per lane to the blanking
TEST(nvidia_displayport, pads_each_lanes_share_of_a_line_to_whole_pixels) {
    edid::timing wxga = {85500, 1366, 70, 143, 213, 768, 3, 3, 24, true, true};
    link_configuration four_rbr_lanes = {4, link_rate::rbr};

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, four_rbr_lanes, wxga, stream));
    expect_stream(stream, {64, 30, 783, 2567});
}

// At NVIDIA's precision of 1 in 100000, the watermark on 4 lanes at HBR2 steps from 22 to 23 between these clocks
TEST(nvidia_displayport, computes_the_watermark_at_nvidias_precision) {
    edid::timing below = AOC_AG241QG4_60_HZ;
    below.pixel_clock_khz = 100990;
    edid::timing above = AOC_AG241QG4_60_HZ;
    above.pixel_clock_khz = 100991;
    link_configuration four_hbr2_lanes = {4, link_rate::hbr2};

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, four_hbr2_lanes, below, stream));
    expect_stream(stream, {64, 22, 821, 13657});

    ASSERT_TRUE(compute_stream(RTX_3080_TO_AOC_AG241QG4, four_hbr2_lanes, above, stream));
    expect_stream(stream, {64, 23, 821, 13657});
}

// Under the normal limits the watermark is raised to 20 rather than 22 and adds 2 rather than 8
TEST(nvidia_displayport, takes_the_normal_watermark_limits_minimum_and_adjustment) {
    link_facts normal_limits = RTX_3080_TO_IOC_25G2S;
    normal_limits.increased_watermark_limits = false;

    stream_parameters stream = {};
    ASSERT_TRUE(compute_stream(normal_limits, {4, link_rate::hbr3}, IOC_25G2S_240_HZ, stream));
    expect_stream(stream, {64, 20, 0x73, 0x849});

    ASSERT_TRUE(compute_stream(normal_limits, {2, link_rate::hbr2}, AOC_AG241QG4_60_HZ, stream));
    expect_stream(stream, {64, 27, 0x14C, 0x163D});
}

TEST(nvidia_displayport, refuses_a_configuration_a_link_cannot_run_or_a_timing_without_a_clock) {
    edid::timing no_clock = AOC_AG241QG4_60_HZ;
    no_clock.pixel_clock_khz = 0;
    link_configuration no_rate = {4, static_cast<link_rate>(0)};

    stream_parameters stream = {1, 2, 3, 4};
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, {0, link_rate::hbr2}, AOC_AG241QG4_60_HZ, stream));
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, {3, link_rate::hbr2}, AOC_AG241QG4_60_HZ, stream));
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, {8, link_rate::hbr2}, AOC_AG241QG4_60_HZ, stream));
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, no_rate, AOC_AG241QG4_60_HZ, stream));
    EXPECT_FALSE(compute_stream(RTX_3080_TO_AOC_AG241QG4, {2, link_rate::hbr2}, no_clock, stream));
    expect_stream(stream, {1, 2, 3, 4});

    link_configuration config = {3, link_rate::hbr};
    EXPECT_FALSE(find_lowest_configuration(RTX_3080_TO_AOC_AG241QG4, no_clock, config));
    expect_configuration(config, 3, link_rate::hbr);
}

// NVIDIA's library decides for a GPU whose links lack FEC support by another rule, which these functions do not follow
TEST(nvidia_displayport, refuses_every_configuration_for_a_gpu_whose_links_lack_fec) {
    link_facts no_fec = RTX_3080_TO_AOC_AG241QG4;
    no_fec.fec_supported = false;

    stream_parameters stream = {1, 2, 3, 4};
    EXPECT_FALSE(compute_stream(no_fec, {2, link_rate::hbr2}, AOC_AG241QG4_60_HZ, stream));
    expect_stream(stream, {1, 2, 3, 4});

    link_configuration config = {3, link_rate::hbr};
    EXPECT_FALSE(find_lowest_configuration(no_fec, AOC_AG241QG4_60_HZ, config));
    expect_configuration(config, 3, link_rate::hbr);
}

TEST(nvidia_displayport, takes_a_link_trained_on_the_lanes_it_runs) {
    EXPECT_TRUE(is_trained({0x77, 0x77, 0x01, 0x00}, 4));
    EXPECT_TRUE(is_trained({0xFF, 0xFF, 0xFF, 0xFF}, 4));
    EXPECT_TRUE(is_trained({0x77, 0x00, 0x01, 0x00}, 2));
    EXPECT_TRUE(is_trained({0x07, 0x00, 0x01, 0x00}, 1));
}

TEST(nvidia_displayport, refuses_a_link_with_a_lane_short_of_trained_or_its_lanes_unaligned) {
    for (uint8_t lane = 0; lane < 4; lane++) {
        for (uint8_t bit = 0; bit < 3; bit++) {
            link_status status = {0xFF, 0xFF, 0xFF, 0xFF};
            uint8_t& pair = lane < 2 ? status.lanes_0_1 : status.lanes_2_3;
            pair = static_cast<uint8_t>(pair & ~(1 << (4 * (lane % 2) + bit)));
            EXPECT_FALSE(is_trained(status, 4));
        }
    }

    EXPECT_FALSE(is_trained({0x77, 0x77, 0xFE, 0xFF}, 4));
}

TEST(nvidia_displayport, takes_the_monitor_in_sync_by_its_first_port) {
    EXPECT_TRUE(is_in_sync({0x00, 0x00, 0x00, 0x01}));
    EXPECT_TRUE(is_in_sync({0x00, 0x00, 0x00, 0xFF}));
    EXPECT_FALSE(is_in_sync({0xFF, 0xFF, 0xFF, 0xFE}));
}
