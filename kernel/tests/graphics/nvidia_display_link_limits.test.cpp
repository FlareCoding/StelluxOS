// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/displayport.h"

TEST_SUITE(nvidia_display_link_limits);

using drivers::nvidia::chip_displayport;
using drivers::nvidia::connector_link_limits;
using drivers::nvidia::display_output;
using drivers::nvidia::dpcd_capabilities;
using drivers::nvidia::is_at_least_revision_1_0;
using drivers::nvidia::monitor_link_limits;
using drivers::nvidia::output_link_facts;
using drivers::nvidia::takes_enhanced_framing;
using drivers::nvidia::displayport::link_facts;
using drivers::nvidia::displayport::link_limits;
using drivers::nvidia::displayport::link_rate;

// A rate byte and the rate NVIDIA's parse takes from it
struct rate_case {
    uint8_t   code;
    link_rate rate;
};

// A lane byte and the most lanes a link trains for it
struct lanes_case {
    uint8_t lane_byte;
    uint8_t lanes;
};

// A receiver capability field's revision byte and whether it gives revision 1.0 or later
struct revision_case {
    uint8_t revision;
    bool    is_at_least_1_0;
};

// DisplayPort's rate codes as NVIDIA's DPCD headers give them, restated so the tests hold the parse to NVIDIA's values
static constexpr uint8_t NVIDIA_RBR  = 0x06;
static constexpr uint8_t NVIDIA_HBR  = 0x0A;
static constexpr uint8_t NVIDIA_HBR2 = 0x14;
static constexpr uint8_t NVIDIA_HBR3 = 0x1E;

// The IOC 25G2S's extended receiver capability field as the RTX 3080 reads it, revision 1.4 with 8.1 Gbps and 4 lanes
// with enhanced framing, and the zeros the card reads for its PHY repeaters
static constexpr dpcd_capabilities IOC_25G2S_DPCD = {
    .is_extended            = true,
    .has_phy_repeater_bytes = true,
    .receiver               = {
        0x14, 0x1E, 0xC4, 0x81, 0x01, 0x00, 0x01, 0x80, 0x02, 0x02, 0x06, 0x00, 0x00, 0x00, 0x81, 0x00,
    },
    .phy_repeaters          = {},
};

// One PHY repeater of revision 1.4 taking 5.4 Gbps and 2 lanes
static constexpr uint8_t TWO_LANE_REPEATER[8] = {0x14, NVIDIA_HBR2, 0x80, 0x00, 0x02, 0x00, 0x00, 0x00};

// The RTX 3080's output 0 as display common reports it, its board supporting four lanes (lane code 3) at 8.1 Gbps
// (link rate code 4)
static constexpr display_output RTX_3080_OUTPUT = {
    .display_id          = 0x100,
    .is_connected        = true,
    .lanes_code          = 3,
    .link_rate_code      = 4,
    .is_limited_to_60_hz = false,
    .prefers_rbr         = false,
    .phy_repeaters       = 0,
};

// A monitor taking `rate_code` and `lane_byte`, read with the PHY repeaters' bytes `repeaters`, none read when null
static dpcd_capabilities monitor_dpcd(uint8_t rate_code, uint8_t lane_byte, const uint8_t* repeaters) {
    dpcd_capabilities dpcd = {};
    dpcd.has_phy_repeater_bytes = repeaters != nullptr;
    dpcd.receiver[0] = 0x14;
    dpcd.receiver[1] = rate_code;
    dpcd.receiver[2] = lane_byte;
    for (uint32_t index = 0; repeaters != nullptr && index < sizeof(dpcd.phy_repeaters); index++) {
        dpcd.phy_repeaters[index] = repeaters[index];
    }

    return dpcd;
}

// Whether `limits` holds `lanes` lanes at `rate`
static bool limits_equal(const link_limits& limits, uint8_t lanes, link_rate rate) {
    return limits.max_lanes == lanes && limits.max_rate == rate;
}

TEST(nvidia_display_link_limits, rtx_3080_connector_takes_four_lanes_at_hbr3) {
    link_limits limits = connector_link_limits(RTX_3080_OUTPUT, link_rate::hbr3);
    EXPECT_TRUE(limits_equal(limits, 4, link_rate::hbr3));
}

// DFP_GET_INFO's LANE and DP_LINK_BW fields are 3 bits wide, so each holds one of 8 codes
static constexpr uint8_t DFP_FIELD_CODES = 8;

// The most lanes a link takes for each lane code, NVIDIA's LANE values: 0 for NONE, 1 for SINGLE, 2 for DUAL, 4 for
// QUAD and for OCT, as no link trains more than 4, and 0 for the codes 5 to 7 NVIDIA defines no count for
static constexpr uint8_t CONNECTOR_LANES[DFP_FIELD_CODES] = {0, 1, 2, 4, 4, 0, 0, 0};

TEST(nvidia_display_link_limits, connector_lanes_follow_the_lane_code) {
    for (uint8_t code = 0; code < DFP_FIELD_CODES; code++) {
        display_output output = RTX_3080_OUTPUT;
        output.lanes_code = code;

        link_limits limits = connector_link_limits(output, link_rate::hbr3);

        EXPECT_TRUE(limits_equal(limits, CONNECTOR_LANES[code], link_rate::hbr3));
    }
}

// The rate each link rate code stands for: RBR for 0, NVIDIA's DP_LINK_BW values 1 to 4 for 1.62, 2.70, 5.40 and 8.10
// Gbps, and RBR for 5 to 7, as NVIDIA's DisplayPort library takes 0 and 5 to 7, the codes NVIDIA defines no rate for
static constexpr link_rate CONNECTOR_RATES[DFP_FIELD_CODES] = {
    link_rate::rbr,
    link_rate::rbr,
    link_rate::hbr,
    link_rate::hbr2,
    link_rate::hbr3,
    link_rate::rbr,
    link_rate::rbr,
    link_rate::rbr,
};

TEST(nvidia_display_link_limits, connector_rate_follows_the_link_rate_code) {
    for (uint8_t code = 0; code < DFP_FIELD_CODES; code++) {
        display_output output = RTX_3080_OUTPUT;
        output.link_rate_code = code;

        link_limits limits = connector_link_limits(output, link_rate::hbr3);

        EXPECT_TRUE(limits_equal(limits, 4, CONNECTOR_RATES[code]));
    }
}

// Codes with bit 3 set read as their low 3 bits, a lane code of 0x0C as 4 lanes and a link rate code of 0x0C as HBR3
TEST(nvidia_display_link_limits, connector_codes_read_as_their_fields_3_bits) {
    display_output output = RTX_3080_OUTPUT;
    output.lanes_code = 0x0C;
    output.link_rate_code = 0x0C;

    link_limits limits = connector_link_limits(output, link_rate::hbr3);

    EXPECT_TRUE(limits_equal(limits, 4, link_rate::hbr3));
}

// The lower of the connector's rate and the GPU's highest is the connector's limit, whichever of the two it is
TEST(nvidia_display_link_limits, the_gpu_rate_caps_the_connector_rate) {
    display_output hbr_output = RTX_3080_OUTPUT;
    hbr_output.link_rate_code = 2;

    link_limits under_hbr2_gpu = connector_link_limits(RTX_3080_OUTPUT, link_rate::hbr2);
    link_limits under_hbr3_gpu = connector_link_limits(hbr_output, link_rate::hbr3);

    EXPECT_TRUE(limits_equal(under_hbr2_gpu, 4, link_rate::hbr2));
    EXPECT_TRUE(limits_equal(under_hbr3_gpu, 4, link_rate::hbr));
}

TEST(nvidia_display_link_limits, ioc_25g2s_takes_four_lanes_at_hbr3_with_enhanced_framing) {
    link_limits limits = monitor_link_limits(IOC_25G2S_DPCD);

    EXPECT_TRUE(limits_equal(limits, 4, link_rate::hbr3));
    EXPECT_TRUE(takes_enhanced_framing(IOC_25G2S_DPCD));
}

TEST(nvidia_display_link_limits, enhanced_framing_is_bit_7_of_the_lane_byte) {
    dpcd_capabilities without = monitor_dpcd(NVIDIA_HBR3, 0x44, nullptr);
    dpcd_capabilities with = monitor_dpcd(NVIDIA_HBR3, 0x84, nullptr);

    EXPECT_FALSE(takes_enhanced_framing(without));
    EXPECT_TRUE(takes_enhanced_framing(with));
}

// Rate bytes and the rate NVIDIA's parse takes from each: RBR, HBR and HBR2 from bits 4 to 0, HBR3 from the whole byte,
// and HBR for every other code
static constexpr rate_case RECEIVER_RATES[] = {
    {NVIDIA_RBR, link_rate::rbr},
    {NVIDIA_HBR, link_rate::hbr},
    {NVIDIA_HBR2, link_rate::hbr2},
    {NVIDIA_HBR3, link_rate::hbr3},
    {0x26, link_rate::rbr},
    {0x34, link_rate::hbr2},
    {0x3E, link_rate::hbr},
    {0x00, link_rate::hbr},
    {0x0C, link_rate::hbr},
    {0x1F, link_rate::hbr},
};

TEST(nvidia_display_link_limits, receiver_rate_follows_nvidia_parse) {
    for (const rate_case& entry : RECEIVER_RATES) {
        dpcd_capabilities dpcd = monitor_dpcd(entry.code, 0x84, nullptr);

        link_limits limits = monitor_link_limits(dpcd);

        EXPECT_TRUE(limits_equal(limits, 4, entry.rate));
    }
}

// Lane bytes and the most lanes a link trains for each: bits 4 to 0 hold the count, 0 and 8 train 4, a count NVIDIA's
// parse rejects counts as 1, and bits 7 to 5 carry other facts
static constexpr lanes_case RECEIVER_LANES[] = {
    {0x00, 4},
    {0x01, 1},
    {0x02, 2},
    {0x03, 1},
    {0x04, 4},
    {0x05, 1},
    {0x08, 4},
    {0x12, 1},
    {0x14, 1},
    {0x1F, 1},
    {0xE2, 2},
    {0xC4, 4},
};

TEST(nvidia_display_link_limits, receiver_lanes_train_as_nvidia_does) {
    for (const lanes_case& entry : RECEIVER_LANES) {
        dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, entry.lane_byte, nullptr);

        link_limits limits = monitor_link_limits(dpcd);

        EXPECT_TRUE(limits_equal(limits, entry.lanes, link_rate::hbr3));
    }
}

// NVIDIA's NV_DPCD_EDP_CONFIG_CAP, the receiver capability field's byte at 0x0D, and its bit declaring eDP display
// control
static constexpr uint32_t EDP_CONFIG_CAP      = 0x0D;
static constexpr uint8_t  EDP_DISPLAY_CONTROL = 0x08;

// A monitor of revision 1.4 declaring eDP display control takes the rate its rate code gives, as no eDP link rate table
// is read
TEST(nvidia_display_link_limits, a_monitor_declaring_edp_display_control_takes_its_rate_code) {
    dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR, 0x84, nullptr);
    dpcd.receiver[EDP_CONFIG_CAP] = EDP_DISPLAY_CONTROL;

    link_limits limits = monitor_link_limits(dpcd);

    EXPECT_TRUE(limits_equal(limits, 4, link_rate::hbr));
}

TEST(nvidia_display_link_limits, a_phy_repeater_caps_the_rate_and_lanes) {
    dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, TWO_LANE_REPEATER);

    link_limits limits = monitor_link_limits(dpcd);

    EXPECT_TRUE(limits_equal(limits, 2, link_rate::hbr2));
}

// A monitor below a counted repeater's rate and lanes keeps its own, the lower of each deciding
TEST(nvidia_display_link_limits, a_phy_repeater_above_the_monitor_leaves_its_limits) {
    uint8_t repeaters[8] = {0x14, NVIDIA_HBR3, 0x80, 0x00, 0x04, 0x00, 0x00, 0x00};
    dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR, 0x82, repeaters);

    link_limits limits = monitor_link_limits(dpcd);

    EXPECT_TRUE(limits_equal(limits, 2, link_rate::hbr));
}

// A lane count of 0 on either side trains 4 lanes, as NVIDIA takes the lower count before reading 0 as 4
TEST(nvidia_display_link_limits, zero_lanes_on_either_side_train_four) {
    uint8_t zero_lane_repeaters[8] = {0x14, NVIDIA_HBR3, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
    dpcd_capabilities two_lane_monitor = monitor_dpcd(NVIDIA_HBR3, 0x82, zero_lane_repeaters);
    dpcd_capabilities zero_lane_monitor = monitor_dpcd(NVIDIA_HBR3, 0x80, TWO_LANE_REPEATER);

    link_limits behind_zero_lanes = monitor_link_limits(two_lane_monitor);
    link_limits behind_two_lanes = monitor_link_limits(zero_lane_monitor);

    EXPECT_TRUE(limits_equal(behind_zero_lanes, 4, link_rate::hbr3));
    EXPECT_TRUE(limits_equal(behind_two_lanes, 4, link_rate::hbr2));
}

// Repeaters below revision 1.4 count for nothing, 0.4 to 0.15 included, which NVIDIA's revision test passes
TEST(nvidia_display_link_limits, phy_repeaters_below_revision_1_4_count_for_nothing) {
    static constexpr uint8_t OLD_REVISIONS[] = {0x00, 0x04, 0x0F, 0x10, 0x13};
    uint8_t repeaters[8] = {0x00, NVIDIA_HBR2, 0x80, 0x00, 0x02, 0x00, 0x00, 0x00};
    for (uint8_t revision : OLD_REVISIONS) {
        repeaters[0] = revision;
        dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, repeaters);

        link_limits limits = monitor_link_limits(dpcd);

        EXPECT_TRUE(limits_equal(limits, 4, link_rate::hbr3));
    }
}

TEST(nvidia_display_link_limits, phy_repeaters_of_revision_2_0_count) {
    uint8_t repeaters[8] = {0x20, NVIDIA_HBR2, 0x80, 0x00, 0x02, 0x00, 0x00, 0x00};
    dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, repeaters);

    link_limits limits = monitor_link_limits(dpcd);

    EXPECT_TRUE(limits_equal(limits, 2, link_rate::hbr2));
}

// Every count byte naming 1 to 8 repeaters caps the monitor's limits, and 0 or a byte of 2 to 8 set bits names none
TEST(nvidia_display_link_limits, phy_repeaters_count_when_their_byte_names_one_count) {
    uint8_t repeaters[8] = {0x14, NVIDIA_HBR2, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
    for (uint8_t bit = 0; bit < 8; bit++) {
        repeaters[2] = static_cast<uint8_t>(1u << bit);
        dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, repeaters);

        link_limits limits = monitor_link_limits(dpcd);

        EXPECT_TRUE(limits_equal(limits, 2, link_rate::hbr2));
    }

    static constexpr uint8_t NO_COUNTS[] = {0x00, 0x81, 0xC0, 0xFF};
    for (uint8_t count : NO_COUNTS) {
        repeaters[2] = count;
        dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, repeaters);

        link_limits limits = monitor_link_limits(dpcd);

        EXPECT_TRUE(limits_equal(limits, 4, link_rate::hbr3));
    }
}

// Repeater lane bytes under a 4-lane monitor and the most lanes a link trains for each: bits 4 to 0 hold the count, 0
// leaves 4, as NVIDIA's library has it, 3 trains 2, and bits 7 to 5 carry other facts
static constexpr lanes_case REPEATER_LANES[] = {
    {0x00, 4},
    {0x03, 2},
    {0x12, 4},
    {0xE2, 2},
};

TEST(nvidia_display_link_limits, phy_repeater_lanes_train_as_nvidia_does) {
    uint8_t repeaters[8] = {0x14, NVIDIA_HBR3, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (const lanes_case& entry : REPEATER_LANES) {
        repeaters[4] = entry.lane_byte;
        dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, repeaters);

        link_limits limits = monitor_link_limits(dpcd);

        EXPECT_TRUE(limits_equal(limits, entry.lanes, link_rate::hbr3));
    }
}

// A repeater's rate code DisplayPort does not define counts as HBR, so it caps a faster monitor at HBR, 0x34 and 0x26
// included, whose bits 4 to 0 the receiver's parse takes as HBR2 and RBR
TEST(nvidia_display_link_limits, unknown_phy_repeater_rate_counts_as_hbr) {
    static constexpr uint8_t UNKNOWN_RATES[] = {0x3E, 0x34, 0x26};
    uint8_t repeaters[8] = {0x14, 0x00, 0x80, 0x00, 0x04, 0x00, 0x00, 0x00};
    for (uint8_t code : UNKNOWN_RATES) {
        repeaters[1] = code;
        dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, repeaters);

        link_limits limits = monitor_link_limits(dpcd);

        EXPECT_TRUE(limits_equal(limits, 4, link_rate::hbr));
    }
}

// Revision bytes and whether each gives revision 1.0 or later: the major revision in bits 7 to 4 decides, so 0.15 is
// below 1.0 however high its minor revision, as NVIDIA's revision test compares the major revision first
static constexpr revision_case RECEIVER_REVISIONS[] = {
    {0x00, false},
    {0x04, false},
    {0x0F, false},
    {0x10, true},
    {0x11, true},
    {0x14, true},
    {0x20, true},
};

TEST(nvidia_display_link_limits, revision_1_0_or_later_follows_the_major_revision) {
    for (const revision_case& entry : RECEIVER_REVISIONS) {
        dpcd_capabilities dpcd = monitor_dpcd(NVIDIA_HBR3, 0x84, nullptr);
        dpcd.receiver[0] = entry.revision;

        EXPECT_EQ(is_at_least_revision_1_0(dpcd), entry.is_at_least_1_0);
    }
}

// An output's link facts take each fact from its source, on three GPU and monitor pairs that no swap or negation of the
// framing, FEC and watermark flags passes: the GPU's rate caps the connector's, a PHY repeater caps the monitor's
TEST(nvidia_display_link_limits, output_link_facts_take_each_fact_from_its_source) {
    chip_displayport fec_gpu = {
        .trains_phy_repeaters       = true,
        .max_rate                   = link_rate::hbr2,
        .fec_supported              = true,
        .increased_watermark_limits = false,
    };

    chip_displayport watermark_gpu = {
        .trains_phy_repeaters       = true,
        .max_rate                   = link_rate::hbr3,
        .fec_supported              = false,
        .increased_watermark_limits = true,
    };

    chip_displayport fec_and_watermark_gpu = {
        .trains_phy_repeaters       = true,
        .max_rate                   = link_rate::hbr3,
        .fec_supported              = true,
        .increased_watermark_limits = true,
    };

    dpcd_capabilities monitor_with_framing = monitor_dpcd(NVIDIA_HBR3, 0x84, nullptr);
    dpcd_capabilities monitor_behind_repeater = monitor_dpcd(NVIDIA_HBR3, 0x84, TWO_LANE_REPEATER);
    dpcd_capabilities monitor_without_framing = monitor_dpcd(NVIDIA_HBR3, 0x04, nullptr);

    link_facts on_fec_gpu = output_link_facts(RTX_3080_OUTPUT, monitor_with_framing, fec_gpu);
    EXPECT_TRUE(limits_equal(on_fec_gpu.connector, 4, link_rate::hbr2));
    EXPECT_TRUE(limits_equal(on_fec_gpu.monitor, 4, link_rate::hbr3));
    EXPECT_TRUE(on_fec_gpu.enhanced_framing);
    EXPECT_TRUE(on_fec_gpu.fec_supported);
    EXPECT_FALSE(on_fec_gpu.increased_watermark_limits);

    link_facts on_watermark_gpu = output_link_facts(RTX_3080_OUTPUT, monitor_behind_repeater, watermark_gpu);
    EXPECT_TRUE(limits_equal(on_watermark_gpu.connector, 4, link_rate::hbr3));
    EXPECT_TRUE(limits_equal(on_watermark_gpu.monitor, 2, link_rate::hbr2));
    EXPECT_TRUE(on_watermark_gpu.enhanced_framing);
    EXPECT_FALSE(on_watermark_gpu.fec_supported);
    EXPECT_TRUE(on_watermark_gpu.increased_watermark_limits);

    link_facts on_both_gpu = output_link_facts(RTX_3080_OUTPUT, monitor_without_framing, fec_and_watermark_gpu);
    EXPECT_TRUE(limits_equal(on_both_gpu.connector, 4, link_rate::hbr3));
    EXPECT_TRUE(limits_equal(on_both_gpu.monitor, 4, link_rate::hbr3));
    EXPECT_FALSE(on_both_gpu.enhanced_framing);
    EXPECT_TRUE(on_both_gpu.fec_supported);
    EXPECT_TRUE(on_both_gpu.increased_watermark_limits);
}
