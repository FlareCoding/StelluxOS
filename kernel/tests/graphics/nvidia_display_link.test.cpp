// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_link.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_link);

using drivers::nvidia::assign_output_link;
using drivers::nvidia::chip_displayport;
using drivers::nvidia::display_output;
using drivers::nvidia::dpcd_capabilities;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::output_link;
using drivers::nvidia::phy_repeater_count;
using drivers::nvidia::read_link_capabilities;
using drivers::nvidia::read_link_status;
using drivers::nvidia::NS_PER_MS;
using drivers::nvidia::train_link;
namespace displayport = drivers::nvidia::displayport;
namespace rm = drivers::nvidia::rm;

// NVIDIA's NV0073_CTRL_CMD_DFP_ASSIGN_SOR, its parameters' size and the offset of its tagged list of display masks and
// types, with the types SINGLE, 2H1OR_PRIMARY and 2H1OR_SECONDARY
static constexpr uint32_t NVIDIA_DFP_ASSIGN_SOR           = 0x731152;
static constexpr size_t   NVIDIA_DFP_ASSIGN_SOR_BYTES     = 80;
static constexpr uint32_t ASSIGN_TAGGED_LIST_OFFSET       = 40;
static constexpr uint32_t NVIDIA_SOR_TYPE_SINGLE          = 1;
static constexpr uint32_t NVIDIA_SOR_TYPE_2H1OR_PRIMARY   = 2;
static constexpr uint32_t NVIDIA_SOR_TYPE_2H1OR_SECONDARY = 3;

// NVIDIA's NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO and its parameters' size and the offsets of its index, type and
// protocol, with the SOR type and the DP_A and DP_B protocols
static constexpr uint32_t NVIDIA_OR_GET_INFO       = 0x73028B;
static constexpr size_t   NVIDIA_OR_GET_INFO_BYTES = 56;
static constexpr uint32_t OR_INDEX_OFFSET          = 8;
static constexpr uint32_t OR_TYPE_OFFSET           = 12;
static constexpr uint32_t OR_PROTOCOL_OFFSET       = 16;
static constexpr uint32_t NVIDIA_OR_TYPE_SOR       = 2;
static constexpr uint32_t NVIDIA_PROTOCOL_SOR_DP_A = 8;
static constexpr uint32_t NVIDIA_PROTOCOL_SOR_DP_B = 9;

// NVIDIA's NV0073_CTRL_CMD_DP_GET_CAPS and its parameters' size and offsets
static constexpr uint32_t NVIDIA_DP_GET_CAPS        = 0x731369;
static constexpr size_t   NVIDIA_DP_GET_CAPS_BYTES  = 56;
static constexpr uint32_t CAPS_SOR_INDEX_OFFSET     = 4;
static constexpr uint32_t CAPS_MAX_LINK_RATE_OFFSET = 8;
static constexpr uint32_t CAPS_VERSIONS_OFFSET      = 12;
static constexpr uint32_t CAPS_WATERMARK_OFFSET     = 22;
static constexpr uint32_t CAPS_FEC_OFFSET           = 25;
static constexpr uint32_t CAPS_PHY_REPEATER_OFFSET  = 26;

// NVIDIA's NV0073_CTRL_CMD_DP_CTRL and its parameters' size and offsets
static constexpr uint32_t NVIDIA_DP_CTRL            = 0x731343;
static constexpr size_t   NVIDIA_DP_CTRL_BYTES      = 28;
static constexpr uint32_t CTRL_CMD_OFFSET           = 8;
static constexpr uint32_t CTRL_DATA_OFFSET          = 12;
static constexpr uint32_t CTRL_RETRY_TIME_OFFSET    = 20;
static constexpr uint32_t NVIDIA_TRAIN_PHY_REPEATER = 1u << 13;

// The command NVIDIA's driver sent the card for the AOC: lane count, link rate, enhanced framing and PHY repeater
// training, and the data for 1 lane at HBR3 toward the sink, as the card trained the IOC 25G2S
static constexpr uint32_t NVIDIA_CARD_COMMAND = 0x2083;
static constexpr uint32_t ONE_HBR3_LANE_DATA  = 0x1E01;

// NVIDIA's NV0073_CTRL_CMD_DP_AUXCH_CTRL, its parameters' offsets and the most data bytes a read gives
static constexpr uint32_t NVIDIA_DP_AUXCH_CTRL = 0x731341;
static constexpr uint32_t AUX_ADDRESS_OFFSET   = 16;
static constexpr uint32_t AUX_DATA_OFFSET      = 20;
static constexpr uint32_t AUX_SIZE_OFFSET      = 36;
static constexpr uint32_t AUX_REPLY_OFFSET     = 40;
static constexpr size_t   AUX_DATA_BYTES       = 16;

// The display common object and display id the tests use, the card's output 0's
static constexpr rm::handle TEST_DISPLAY_COMMON = 0x0005000B;
static constexpr uint32_t   TEST_DISPLAY_ID     = 0x100;

// A request count that refuses every request
static constexpr uint32_t EVERY_REQUEST = 0xFFFFFFFF;

// The most bytes of a request's parameters a call keeps
static constexpr size_t PARAMS_BYTES = 128;

// The byte each reply is dirtied with before the double writes its fields
static constexpr uint8_t STALE_BYTE = 0xA5;

static uint32_t read_u32(const uint8_t* bytes, uint32_t offset) {
    uint32_t value = 0;
    string::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static void write_u32(uint8_t* bytes, uint32_t offset, uint32_t value) {
    string::memcpy(bytes + offset, &value, sizeof(value));
}

// The text logged while `LINK_LOG_BACKEND` is set
static char     s_link_log[4096] = {};
static uint32_t s_link_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_link_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_link_log_bytes < sizeof(s_link_log); i++) {
        s_link_log[s_link_log_bytes++] = data[i];
    }
}

static constexpr log::backend LINK_LOG_BACKEND = {.write = keep_link_log};

// Whether the kept text holds `text` anywhere
static bool link_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_link_log_bytes; start++) {
        if (string::memcmp(s_link_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// The status bytes the double answers at each address it knows, every other address reading 0
struct status_bytes {
    uint32_t address;
    uint8_t  bytes[3];
};

// A firmware that records each control as sent, refuses the first `refusals` requests of `refused_command`, and answers
// the assignment, output resource facts, capabilities, training and AUX reads from its fields
class link_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 16;

    struct call {
        rm::handle object;
        uint32_t   command;
        size_t     size;
        uint8_t    params[PARAMS_BYTES];
    };

    call     calls[MAX_CALLS] = {};
    uint32_t call_count = 0;

    uint32_t refused_command = 0;
    uint32_t refusals = EVERY_REQUEST;
    int32_t  refusal = ERR_FIRMWARE;
    uint32_t trained_retry_time_ms = 0; // the wait an accepted training's reply asks for

    uint32_t assigned_masks[4] = {TEST_DISPLAY_ID, 0, 0, 0};
    uint32_t assigned_types[4] = {NVIDIA_SOR_TYPE_SINGLE, 0, 0, 0};
    uint32_t resource_index = 0;
    uint32_t resource_type = NVIDIA_OR_TYPE_SOR;
    uint32_t protocol = NVIDIA_PROTOCOL_SOR_DP_A;

    uint32_t max_link_rate = 4;
    uint32_t versions = 0x3;
    uint8_t  watermark_limits = 1;
    uint8_t  fec_supported = 1;
    uint8_t  trains_phy_repeaters = 1;

    status_bytes statuses[4] = {};

    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override {
        return OK;
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        uint32_t made = 0;
        for (uint32_t i = 0; i < call_count && i < MAX_CALLS; i++) {
            made += calls[i].command == command ? 1 : 0;
        }

        if (call_count < MAX_CALLS) {
            call& recorded = calls[call_count];
            recorded.object = object;
            recorded.command = command;
            recorded.size = size;
            string::memcpy(recorded.params, params, size < PARAMS_BYTES ? size : PARAMS_BYTES);
        }

        call_count++;

        uint8_t* bytes = static_cast<uint8_t*>(params);
        if (command == refused_command && made < refusals) {
            return refusal;
        }

        answer(command, bytes, size);

        return OK;
    }

    int32_t release(rm::handle) override {
        return OK;
    }

    rm::handle new_handle() override {
        return 0;
    }

    rm::handle client() const override {
        return 0;
    }

    int32_t internal_control(uint32_t, void*, size_t) override {
        return OK;
    }

    // How many of the calls so far are controls of `command`
    uint32_t count(uint32_t command) const {
        uint32_t made = 0;
        for (uint32_t i = 0; i < call_count && i < MAX_CALLS; i++) {
            made += calls[i].command == command ? 1 : 0;
        }

        return made;
    }

private:
    // Every byte the reply does not give is dirtied, so a later request reusing this stack shows if not zeroed
    void answer(uint32_t command, uint8_t* bytes, size_t size) const {
        if (command == NVIDIA_DP_AUXCH_CTRL) {
            answer_aux(bytes, size);
            return;
        }

        string::memset(bytes, STALE_BYTE, size);

        if (command == NVIDIA_DFP_ASSIGN_SOR) {
            for (uint32_t i = 0; i < 4; i++) {
                write_u32(bytes, ASSIGN_TAGGED_LIST_OFFSET + 8 * i, assigned_masks[i]);
                write_u32(bytes, ASSIGN_TAGGED_LIST_OFFSET + 8 * i + 4, assigned_types[i]);
            }
        } else if (command == NVIDIA_OR_GET_INFO) {
            write_u32(bytes, OR_INDEX_OFFSET, resource_index);
            write_u32(bytes, OR_TYPE_OFFSET, resource_type);
            write_u32(bytes, OR_PROTOCOL_OFFSET, protocol);
        } else if (command == NVIDIA_DP_GET_CAPS) {
            write_u32(bytes, CAPS_MAX_LINK_RATE_OFFSET, max_link_rate);
            write_u32(bytes, CAPS_VERSIONS_OFFSET, versions);
            bytes[CAPS_WATERMARK_OFFSET] = watermark_limits;
            bytes[CAPS_FEC_OFFSET] = fec_supported;
            bytes[CAPS_PHY_REPEATER_OFFSET] = trains_phy_repeaters;
        } else if (command == NVIDIA_DP_CTRL) {
            write_u32(bytes, CTRL_RETRY_TIME_OFFSET, trained_retry_time_ms);
        }
    }

    // Takes the address and size before the dirtying, then acknowledges the read with the status bytes there, or 0s
    void answer_aux(uint8_t* bytes, size_t size) const {
        uint32_t address = read_u32(bytes, AUX_ADDRESS_OFFSET);
        uint32_t asked = read_u32(bytes, AUX_SIZE_OFFSET) + 1;

        string::memset(bytes, STALE_BYTE, size);
        string::memset(bytes + AUX_DATA_OFFSET, 0, AUX_DATA_BYTES);
        for (const status_bytes& status : statuses) {
            if (status.address == address && address != 0) {
                string::memcpy(bytes + AUX_DATA_OFFSET, status.bytes, sizeof(status.bytes));
            }
        }

        write_u32(bytes, AUX_SIZE_OFFSET, asked);
        write_u32(bytes, AUX_REPLY_OFFSET, 0);
    }
};

// The card's output 0 as display common reports it, with no PHY repeater
static constexpr display_output CARD_OUTPUT = {
    .display_id          = TEST_DISPLAY_ID,
    .is_connected        = true,
    .lanes_code          = 3,
    .link_rate_code      = 4,
    .is_limited_to_60_hz = false,
    .prefers_rbr         = false,
    .phy_repeaters       = 0,
};

// A monitor's capabilities, DPCD 1.4 at HBR3 over four lanes with enhanced framing, read without PHY repeaters
static dpcd_capabilities hbr3_dpcd() {
    dpcd_capabilities dpcd = {};
    dpcd.receiver[0] = 0x14;
    dpcd.receiver[1] = 0x1E;
    dpcd.receiver[2] = 0x84;
    return dpcd;
}

// The same monitor's capabilities behind `repeaters` PHY repeaters of revision 1.4
static dpcd_capabilities hbr3_dpcd_behind(uint8_t repeaters) {
    dpcd_capabilities dpcd = hbr3_dpcd();
    dpcd.has_phy_repeater_bytes = true;
    dpcd.phy_repeaters[0] = 0x14;
    dpcd.phy_repeaters[1] = 0x1E;
    dpcd.phy_repeaters[2] = static_cast<uint8_t>(0x80 >> (repeaters - 1));
    dpcd.phy_repeaters[4] = 0x04;

    return dpcd;
}

static constexpr displayport::link_configuration ONE_HBR3_LANE  = {.lanes = 1, .rate = displayport::link_rate::hbr3};
static constexpr displayport::link_configuration TWO_HBR2_LANES = {.lanes = 2, .rate = displayport::link_rate::hbr2};

TEST(nvidia_display_link, assigns_through_the_crossbar_in_nvidias_layout) {
    link_firmware earlier;
    output_link earlier_link = {};
    bool earlier_is_assigned = false;

    link_firmware fw;
    fw.assigned_masks[0] = 0;
    fw.assigned_masks[2] = TEST_DISPLAY_ID | 0x800;
    fw.assigned_types[2] = NVIDIA_SOR_TYPE_SINGLE;
    fw.protocol = NVIDIA_PROTOCOL_SOR_DP_B;

    output_link link = {};
    bool is_assigned = false;

    // The earlier assignment leaves its dirtied replies in the frame the next one's requests take, so a request not
    // zeroed carries them
    ASSERT_EQ(
        assign_output_link(earlier, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, earlier_link, earlier_is_assigned),
        OK
    );
    ASSERT_EQ(assign_output_link(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);

    EXPECT_TRUE(is_assigned);
    EXPECT_EQ(link.index, 2u);
    EXPECT_EQ(link.sublink, 1u);

    ASSERT_EQ(fw.call_count, 2u);
    EXPECT_EQ(fw.calls[0].object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(fw.calls[0].command, NVIDIA_DFP_ASSIGN_SOR);
    EXPECT_EQ(fw.calls[0].size, NVIDIA_DFP_ASSIGN_SOR_BYTES);

    EXPECT_EQ(read_u32(fw.calls[0].params, 0), 0u);
    EXPECT_EQ(read_u32(fw.calls[0].params, 4), TEST_DISPLAY_ID);
    for (uint32_t offset = 8; offset < NVIDIA_DFP_ASSIGN_SOR_BYTES; offset++) {
        EXPECT_EQ(fw.calls[0].params[offset], 0u);
    }

    EXPECT_EQ(fw.calls[1].command, NVIDIA_OR_GET_INFO);
    EXPECT_EQ(fw.calls[1].size, NVIDIA_OR_GET_INFO_BYTES);

    EXPECT_EQ(read_u32(fw.calls[1].params, 0), 0u);
    EXPECT_EQ(read_u32(fw.calls[1].params, 4), TEST_DISPLAY_ID);
    for (uint32_t offset = OR_INDEX_OFFSET; offset < NVIDIA_OR_GET_INFO_BYTES; offset++) {
        EXPECT_EQ(fw.calls[1].params[offset], 0u);
    }
}

TEST(nvidia_display_link, takes_the_resource_driving_the_display_id_alone_or_as_primary) {
    link_firmware fw;
    fw.assigned_masks[0] = 0x800;
    fw.assigned_types[0] = NVIDIA_SOR_TYPE_SINGLE;

    fw.assigned_masks[1] = TEST_DISPLAY_ID;
    fw.assigned_types[1] = NVIDIA_SOR_TYPE_2H1OR_SECONDARY;

    fw.assigned_masks[3] = TEST_DISPLAY_ID;
    fw.assigned_types[3] = NVIDIA_SOR_TYPE_2H1OR_PRIMARY;

    output_link link = {};
    bool is_assigned = false;
    ASSERT_EQ(assign_output_link(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);

    EXPECT_TRUE(is_assigned);
    EXPECT_EQ(link.index, 3u);
    EXPECT_EQ(link.sublink, 0u);
}

// The crossbar's first entry alone names the display id, then two entries do and the second is taken, the facts' index
// taken in neither
TEST(nvidia_display_link, takes_the_last_entry_naming_a_resource_for_the_display_id) {
    link_firmware first;
    first.assigned_masks[0] = TEST_DISPLAY_ID;
    first.assigned_types[0] = NVIDIA_SOR_TYPE_SINGLE;
    first.resource_index = 3;

    output_link link = {};
    bool is_assigned = false;
    ASSERT_EQ(assign_output_link(first, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);
    EXPECT_TRUE(is_assigned);
    EXPECT_EQ(link.index, 0u);

    link_firmware two;
    two.assigned_masks[0] = 0;
    two.assigned_masks[1] = TEST_DISPLAY_ID;
    two.assigned_types[1] = NVIDIA_SOR_TYPE_SINGLE;
    two.assigned_masks[2] = TEST_DISPLAY_ID;
    two.assigned_types[2] = NVIDIA_SOR_TYPE_2H1OR_PRIMARY;

    is_assigned = false;
    ASSERT_EQ(assign_output_link(two, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);
    EXPECT_TRUE(is_assigned);
    EXPECT_EQ(link.index, 2u);
}

TEST(nvidia_display_link, assigns_none_when_the_crossbar_names_no_resource_or_refuses) {
    output_link link = {};
    bool is_assigned = true;

    link_firmware unnamed;
    unnamed.assigned_types[0] = NVIDIA_SOR_TYPE_2H1OR_SECONDARY;
    ASSERT_EQ(assign_output_link(unnamed, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
    EXPECT_EQ(unnamed.call_count, 1u);

    link_firmware refusing;
    refusing.refused_command = NVIDIA_DFP_ASSIGN_SOR;
    is_assigned = true;
    ASSERT_EQ(assign_output_link(refusing, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
    EXPECT_EQ(refusing.call_count, 1u);

    link_firmware failing;
    failing.refused_command = NVIDIA_DFP_ASSIGN_SOR;
    failing.refusal = ERR_TIMEOUT;
    EXPECT_EQ(assign_output_link(failing, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), ERR_TIMEOUT);
    EXPECT_FALSE(is_assigned);
}

TEST(nvidia_display_link, takes_the_index_from_the_resource_facts_without_the_crossbar) {
    link_firmware fw;
    fw.resource_index = 3;

    output_link link = {};
    bool is_assigned = false;
    ASSERT_EQ(assign_output_link(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, false, link, is_assigned), OK);

    EXPECT_TRUE(is_assigned);
    EXPECT_EQ(link.index, 3u);
    EXPECT_EQ(link.sublink, 0u);
    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_EQ(fw.calls[0].command, NVIDIA_OR_GET_INFO);
}

TEST(nvidia_display_link, assigns_none_for_another_resource_type_index_or_protocol) {
    output_link link = {};
    bool is_assigned = true;

    link_firmware other_type;
    other_type.resource_type = 1;
    ASSERT_EQ(assign_output_link(other_type, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, false, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);

    link_firmware past_the_resources;
    past_the_resources.resource_index = 4;
    is_assigned = true;
    ASSERT_EQ(
        assign_output_link(past_the_resources, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, false, link, is_assigned),
        OK
    );
    EXPECT_FALSE(is_assigned);

    link_firmware other_protocol;
    other_protocol.protocol = 1;
    is_assigned = true;
    ASSERT_EQ(assign_output_link(other_protocol, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);

    link_firmware refusing;
    refusing.refused_command = NVIDIA_OR_GET_INFO;
    is_assigned = true;
    ASSERT_EQ(assign_output_link(refusing, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
}

// Asks for the link of `TEST_DISPLAY_ID`, with the log kept
static int32_t assign_logged(link_firmware& fw, bool has_crossbar, output_link& link, bool& is_assigned) {
    s_link_log_bytes = 0;
    log::set_backend(&LINK_LOG_BACKEND);
    int32_t rc = assign_output_link(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, has_crossbar, link, is_assigned);
    log::set_backend(nullptr);

    return rc;
}

// Each refused resource's line gives the facts' index, type and protocol, each a value no other field holds
TEST(nvidia_display_link, logs_the_index_type_and_protocol_of_a_resource_it_refuses) {
    output_link link = {};
    bool is_assigned = true;

    link_firmware other_type;
    other_type.resource_index = 3;
    other_type.resource_type = 1;

    ASSERT_EQ(assign_logged(other_type, false, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
    EXPECT_TRUE(link_log_contains(
        "nvidia: display id 0x100 is driven by output resource 3 of type 1, not a serial one, so "
    ));
    EXPECT_FALSE(link_log_contains("through its link"));

    link_firmware past_the_resources;
    past_the_resources.resource_index = 5;

    ASSERT_EQ(assign_logged(past_the_resources, false, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
    EXPECT_TRUE(link_log_contains(
        "nvidia: display id 0x100 is driven by serial output resource 5, past the 4 display common assigns, so "
    ));
    EXPECT_FALSE(link_log_contains("through its link"));

    link_firmware other_protocol;
    other_protocol.resource_index = 1;
    other_protocol.protocol = 7;

    ASSERT_EQ(assign_logged(other_protocol, false, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
    EXPECT_TRUE(link_log_contains(
        "nvidia: display id 0x100's output resource 1 drives protocol 7, not DisplayPort, so "
    ));
    EXPECT_FALSE(link_log_contains("through its link"));
}

// A crossbar reply naming no resource for the display id and a refused output resource facts request each log their
// line, checked up to the words saying the facts are not read, and neither logs a link
TEST(nvidia_display_link, logs_a_crossbar_naming_no_resource_and_refused_resource_facts) {
    output_link link = {};
    bool is_assigned = true;

    link_firmware unnamed;
    unnamed.assigned_types[0] = NVIDIA_SOR_TYPE_2H1OR_SECONDARY;

    ASSERT_EQ(assign_logged(unnamed, true, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
    EXPECT_TRUE(link_log_contains(
        "nvidia: display common's assignment for display id 0x100 names no output resource driving it on its own or "
        "as primary, so the link's DisplayPort facts are not read"
    ));
    EXPECT_FALSE(link_log_contains("through its link"));

    link_firmware refusing;
    refusing.refused_command = NVIDIA_OR_GET_INFO;
    is_assigned = true;

    ASSERT_EQ(assign_logged(refusing, true, link, is_assigned), OK);
    EXPECT_FALSE(is_assigned);
    EXPECT_TRUE(link_log_contains(
        "nvidia: display common refused the output resource facts of display id 0x100, so the link's DisplayPort "
        "facts are not read"
    ));
    EXPECT_FALSE(link_log_contains("through its link"));
}

// A request failing other than by display common's refusal gives its error, for the resource facts and for the links'
// DisplayPort facts
TEST(nvidia_display_link, gives_the_error_of_a_request_failing_other_than_by_a_refusal) {
    link_firmware facts;
    facts.refused_command = NVIDIA_OR_GET_INFO;
    facts.refusal = ERR_TIMEOUT;

    output_link link = {};
    bool is_assigned = true;
    EXPECT_EQ(assign_output_link(facts, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, false, link, is_assigned), ERR_TIMEOUT);
    EXPECT_FALSE(is_assigned);

    link_firmware capabilities;
    capabilities.refused_command = NVIDIA_DP_GET_CAPS;
    capabilities.refusal = ERR_TIMEOUT;

    chip_displayport gpu = {};
    bool supports_1_2 = false;
    bool is_read = true;
    EXPECT_EQ(read_link_capabilities(capabilities, TEST_DISPLAY_COMMON, 0, gpu, supports_1_2, is_read), ERR_TIMEOUT);
    EXPECT_FALSE(is_read);
}

TEST(nvidia_display_link, reads_the_cards_link_capabilities_in_nvidias_layout) {
    link_firmware earlier;
    chip_displayport earlier_gpu = {};
    bool earlier_supports_1_2 = false;
    bool earlier_is_read = false;

    link_firmware fw;
    chip_displayport gpu = {};
    bool supports_1_2 = false;
    bool is_read = false;

    // The earlier read leaves its dirtied reply in the frame the next read's request takes, so a request not zeroed
    // carries it
    ASSERT_EQ(
        read_link_capabilities(earlier, TEST_DISPLAY_COMMON, 2, earlier_gpu, earlier_supports_1_2, earlier_is_read),
        OK
    );
    ASSERT_EQ(read_link_capabilities(fw, TEST_DISPLAY_COMMON, 2, gpu, supports_1_2, is_read), OK);

    EXPECT_TRUE(is_read);
    EXPECT_TRUE(supports_1_2);
    EXPECT_EQ(static_cast<uint32_t>(gpu.max_rate), 0x1Eu);
    EXPECT_TRUE(gpu.fec_supported);
    EXPECT_TRUE(gpu.increased_watermark_limits);
    EXPECT_TRUE(gpu.trains_phy_repeaters);

    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_EQ(fw.calls[0].command, NVIDIA_DP_GET_CAPS);
    EXPECT_EQ(fw.calls[0].size, NVIDIA_DP_GET_CAPS_BYTES);

    EXPECT_EQ(read_u32(fw.calls[0].params, 0), 0u);
    EXPECT_EQ(read_u32(fw.calls[0].params, CAPS_SOR_INDEX_OFFSET), 2u);
    for (uint32_t offset = CAPS_SOR_INDEX_OFFSET + 4; offset < NVIDIA_DP_GET_CAPS_BYTES; offset++) {
        EXPECT_EQ(fw.calls[0].params[offset], 0u);
    }
}

TEST(nvidia_display_link, counts_capability_bytes_set_only_at_1_and_other_rates_as_rbr) {
    static constexpr uint32_t RATES[][2] = {{0, 0x06}, {1, 0x06}, {2, 0x0A}, {3, 0x14}, {5, 0x06}, {7, 0x06}};
    for (const uint32_t* rate : RATES) {
        link_firmware fw;
        fw.max_link_rate = rate[0];
        fw.versions = 0x2;
        fw.watermark_limits = 0x81;
        fw.fec_supported = 0xFF;
        fw.trains_phy_repeaters = 0x81;

        chip_displayport gpu = {};
        bool supports_1_2 = true;
        bool is_read = false;
        ASSERT_EQ(read_link_capabilities(fw, TEST_DISPLAY_COMMON, 0, gpu, supports_1_2, is_read), OK);

        EXPECT_TRUE(is_read);
        EXPECT_EQ(static_cast<uint32_t>(gpu.max_rate), rate[1]);
        EXPECT_FALSE(supports_1_2);
        EXPECT_FALSE(gpu.increased_watermark_limits);
        EXPECT_FALSE(gpu.fec_supported);
        EXPECT_FALSE(gpu.trains_phy_repeaters);
    }
}

// DisplayPort 1.2 is read from the versions' bit 0 alone: 0x01 reads as supporting it and 0xFE, bits 7 to 1 set, as not
TEST(nvidia_display_link, reads_displayport_1_2_from_the_versions_bit_0_alone) {
    static constexpr uint32_t VERSIONS[][2] = {{0x01, 1}, {0xFE, 0}};
    for (const uint32_t* version : VERSIONS) {
        link_firmware fw;
        fw.versions = version[0];

        chip_displayport gpu = {};
        bool supports_1_2 = version[1] == 0;
        bool is_read = false;
        ASSERT_EQ(read_link_capabilities(fw, TEST_DISPLAY_COMMON, 0, gpu, supports_1_2, is_read), OK);

        EXPECT_TRUE(is_read);
        EXPECT_EQ(supports_1_2, version[1] == 1);
    }
}

TEST(nvidia_display_link, reads_the_highest_rate_code_as_its_fields_3_bits) {
    link_firmware fw;
    fw.max_link_rate = 0x0C;

    chip_displayport gpu = {};
    bool supports_1_2 = false;
    bool is_read = false;
    ASSERT_EQ(read_link_capabilities(fw, TEST_DISPLAY_COMMON, 2, gpu, supports_1_2, is_read), OK);

    EXPECT_TRUE(is_read);
    EXPECT_EQ(static_cast<uint32_t>(gpu.max_rate), 0x1Eu);
}

TEST(nvidia_display_link, gives_no_capabilities_when_the_request_is_refused) {
    link_firmware fw;
    fw.refused_command = NVIDIA_DP_GET_CAPS;

    chip_displayport gpu = {};
    bool supports_1_2 = false;
    bool is_read = true;
    ASSERT_EQ(read_link_capabilities(fw, TEST_DISPLAY_COMMON, 0, gpu, supports_1_2, is_read), OK);
    EXPECT_FALSE(is_read);
}

TEST(nvidia_display_link, trains_the_cards_link_with_nvidias_command) {
    link_firmware fw;

    bool is_trained = false;
    ASSERT_EQ(
        train_link(fw, TEST_DISPLAY_COMMON, CARD_OUTPUT, hbr3_dpcd(), true, ONE_HBR3_LANE, true, is_trained),
        OK
    );

    EXPECT_TRUE(is_trained);
    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_EQ(fw.calls[0].object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(fw.calls[0].command, NVIDIA_DP_CTRL);
    EXPECT_EQ(fw.calls[0].size, NVIDIA_DP_CTRL_BYTES);

    EXPECT_EQ(read_u32(fw.calls[0].params, 0), 0u);
    EXPECT_EQ(read_u32(fw.calls[0].params, 4), TEST_DISPLAY_ID);
    EXPECT_EQ(read_u32(fw.calls[0].params, CTRL_CMD_OFFSET), NVIDIA_CARD_COMMAND);
    EXPECT_EQ(read_u32(fw.calls[0].params, CTRL_DATA_OFFSET), ONE_HBR3_LANE_DATA);
    for (uint32_t offset = CTRL_DATA_OFFSET + 4; offset < NVIDIA_DP_CTRL_BYTES; offset++) {
        EXPECT_EQ(fw.calls[0].params[offset], 0u);
    }
}

// The lane count goes in the data's low bits below the rate's code: 2 lanes at HBR2 send 0x1402, 4 lanes at RBR 0x0604
TEST(nvidia_display_link, trains_each_lane_count_in_the_datas_low_bits) {
    struct training {
        displayport::link_configuration config;
        uint32_t                        data;
    };

    static constexpr training TRAININGS[] = {
        {TWO_HBR2_LANES, 0x1402},
        {{.lanes = 4, .rate = displayport::link_rate::rbr}, 0x0604},
    };

    for (const training& sent : TRAININGS) {
        link_firmware fw;
        bool is_trained = false;
        int32_t rc = train_link(fw, TEST_DISPLAY_COMMON, CARD_OUTPUT, hbr3_dpcd(), true, sent.config, true, is_trained);

        ASSERT_EQ(rc, OK);
        EXPECT_TRUE(is_trained);
        ASSERT_EQ(fw.call_count, 1u);
        EXPECT_EQ(read_u32(fw.calls[0].params, CTRL_DATA_OFFSET), sent.data);
    }
}

TEST(nvidia_display_link, trains_each_phy_repeater_from_the_gpu_before_the_sink) {
    link_firmware fw;
    display_output output = CARD_OUTPUT;
    output.phy_repeaters = 2;

    bool is_trained = false;
    ASSERT_EQ(
        train_link(fw, TEST_DISPLAY_COMMON, output, hbr3_dpcd_behind(2), true, ONE_HBR3_LANE, false, is_trained),
        OK
    );

    EXPECT_TRUE(is_trained);
    ASSERT_EQ(fw.call_count, 3u);

    // Each request after the first takes the frame the one before left holding its dirtied reply, nothing running
    // between them, so a request not zeroed carries that reply
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t target = 2 - i;
        const uint8_t* params = fw.calls[i].params;

        EXPECT_EQ(read_u32(params, 0), 0u);
        EXPECT_EQ(read_u32(params, CTRL_CMD_OFFSET), 0x3u | NVIDIA_TRAIN_PHY_REPEATER);
        EXPECT_EQ(read_u32(params, CTRL_DATA_OFFSET), ONE_HBR3_LANE_DATA | (target << 19));
        for (uint32_t offset = CTRL_DATA_OFFSET + 4; offset < NVIDIA_DP_CTRL_BYTES; offset++) {
            EXPECT_EQ(params[offset], 0u);
        }
    }
}

TEST(nvidia_display_link, trains_only_the_sink_unless_the_firmware_counts_the_dpcds_repeaters) {
    display_output output = CARD_OUTPUT;
    output.phy_repeaters = 1;

    link_firmware other_count;
    bool is_trained = false;
    ASSERT_EQ(
        train_link(
            other_count,
            TEST_DISPLAY_COMMON,
            output,
            hbr3_dpcd_behind(2),
            true,
            ONE_HBR3_LANE,
            true,
            is_trained
        ),
        OK
    );

    EXPECT_TRUE(is_trained);
    ASSERT_EQ(other_count.call_count, 1u);
    EXPECT_EQ(read_u32(other_count.calls[0].params, CTRL_CMD_OFFSET), 0x83u);
    EXPECT_EQ(read_u32(other_count.calls[0].params, CTRL_DATA_OFFSET), ONE_HBR3_LANE_DATA);

    output.phy_repeaters = 2;
    link_firmware untrained_repeaters;
    ASSERT_EQ(
        train_link(
            untrained_repeaters,
            TEST_DISPLAY_COMMON,
            output,
            hbr3_dpcd_behind(2),
            false,
            ONE_HBR3_LANE,
            true,
            is_trained
        ),
        OK
    );
    ASSERT_EQ(untrained_repeaters.call_count, 1u);
    EXPECT_EQ(read_u32(untrained_repeaters.calls[0].params, CTRL_CMD_OFFSET), 0x83u);
}

// A refused training is sent once and leaves the link untrained
TEST(nvidia_display_link, stops_at_a_refused_training_sending_it_once) {
    link_firmware refusing;
    refusing.refused_command = NVIDIA_DP_CTRL;

    bool is_trained = true;
    ASSERT_EQ(
        train_link(refusing, TEST_DISPLAY_COMMON, CARD_OUTPUT, hbr3_dpcd(), true, ONE_HBR3_LANE, true, is_trained),
        OK
    );
    EXPECT_FALSE(is_trained);
    EXPECT_EQ(refusing.call_count, 1u);
}

// A refused training's line gives the refused request's lanes, rate, command and data, each a value no other field
// holds: the PHY repeater's request at 2 lanes at HBR2 without enhanced framing, its data naming the repeater
TEST(nvidia_display_link, logs_the_lanes_rate_command_and_data_of_a_refused_training) {
    link_firmware refusing;
    refusing.refused_command = NVIDIA_DP_CTRL;
    display_output output = CARD_OUTPUT;
    output.phy_repeaters = 1;

    bool is_trained = true;
    s_link_log_bytes = 0;
    log::set_backend(&LINK_LOG_BACKEND);
    int32_t rc = train_link(
        refusing,
        TEST_DISPLAY_COMMON,
        output,
        hbr3_dpcd_behind(1),
        true,
        TWO_HBR2_LANES,
        false,
        is_trained
    );
    log::set_backend(nullptr);

    EXPECT_EQ(rc, OK);
    EXPECT_FALSE(is_trained);
    EXPECT_EQ(refusing.call_count, 1u);
    EXPECT_TRUE(link_log_contains(
        "nvidia: display common refused to train display id 0x100's link at 2 lanes at DisplayPort rate 0x14 (command "
        "0x2003, data 0x81402), so "
    ));
}

// The wait an accepted training's reply asks for, long enough for the clock to tell it from none and short enough for
// an emulator without acceleration
static constexpr uint32_t TRAIN_WAIT_MS = 5;

// An accepted reply asking for a wait is waited out before the training goes on
TEST(nvidia_display_link, waits_out_the_wait_an_accepted_training_reply_asks_for) {
    dpcd_capabilities dpcd = hbr3_dpcd();
    link_firmware accepted;
    accepted.trained_retry_time_ms = TRAIN_WAIT_MS;
    bool is_trained = false;

    uint64_t start = clock::now_ns();
    int32_t rc = train_link(accepted, TEST_DISPLAY_COMMON, CARD_OUTPUT, dpcd, true, ONE_HBR3_LANE, true, is_trained);
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_EQ(rc, OK);
    EXPECT_TRUE(is_trained);
    EXPECT_EQ(accepted.call_count, 1u);
    EXPECT_GE(elapsed, TRAIN_WAIT_MS * NS_PER_MS);
}

TEST(nvidia_display_link, counts_the_phy_repeaters_nvidia_counts) {
    EXPECT_EQ(phy_repeater_count(hbr3_dpcd()), 0u);
    for (uint8_t repeaters = 1; repeaters <= 8; repeaters++) {
        EXPECT_EQ(phy_repeater_count(hbr3_dpcd_behind(repeaters)), repeaters);
    }

    dpcd_capabilities two_bits = hbr3_dpcd_behind(1);
    two_bits.phy_repeaters[2] = 0xC0;
    EXPECT_EQ(phy_repeater_count(two_bits), 0u);

    dpcd_capabilities old_revision = hbr3_dpcd_behind(1);
    old_revision.phy_repeaters[0] = 0x13;
    EXPECT_EQ(phy_repeater_count(old_revision), 0u);
}

// A training or status request failing other than by display common's refusal gives its error
TEST(nvidia_display_link, gives_the_error_of_a_training_or_status_request_failing_other_than_by_a_refusal) {
    link_firmware training;
    training.refused_command = NVIDIA_DP_CTRL;
    training.refusal = ERR_TIMEOUT;

    bool is_trained = true;
    EXPECT_EQ(
        train_link(training, TEST_DISPLAY_COMMON, CARD_OUTPUT, hbr3_dpcd(), true, ONE_HBR3_LANE, true, is_trained),
        ERR_TIMEOUT
    );
    EXPECT_FALSE(is_trained);

    link_firmware status_read;
    status_read.refused_command = NVIDIA_DP_AUXCH_CTRL;
    status_read.refusal = ERR_TIMEOUT;

    displayport::link_status status = {};
    bool is_read = true;
    EXPECT_EQ(
        read_link_status(status_read, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, hbr3_dpcd(), true, status, is_read),
        ERR_TIMEOUT
    );
    EXPECT_FALSE(is_read);
}

TEST(nvidia_display_link, reads_the_status_nvidia_reads_with_each_repeaters_anded_in) {
    link_firmware fw;
    fw.statuses[0] = {.address = 0x0200C, .bytes = {0x77, 0x77, 0x01}};
    fw.statuses[1] = {.address = 0xF0030, .bytes = {0x07, 0x77, 0x81}};
    fw.statuses[2] = {.address = 0xF0080, .bytes = {0x77, 0x73, 0x01}};

    displayport::link_status status = {};
    bool is_read = false;
    dpcd_capabilities dpcd = hbr3_dpcd_behind(2);
    ASSERT_EQ(read_link_status(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, dpcd, true, status, is_read), OK);

    EXPECT_TRUE(is_read);
    EXPECT_EQ(status.lanes_0_1, 0x07u);
    EXPECT_EQ(status.lanes_2_3, 0x73u);
    EXPECT_EQ(status.lane_align, 0x01u);
    EXPECT_EQ(status.sink_status, 0u);

    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_EQ(read_u32(fw.calls[0].params, AUX_ADDRESS_OFFSET), 0xF0080u);
    EXPECT_EQ(read_u32(fw.calls[1].params, AUX_ADDRESS_OFFSET), 0xF0030u);
    EXPECT_EQ(read_u32(fw.calls[2].params, AUX_ADDRESS_OFFSET), 0x0200Cu);
    for (uint32_t i = 0; i < 3; i++) {
        EXPECT_EQ(fw.calls[i].command, NVIDIA_DP_AUXCH_CTRL);
        EXPECT_EQ(read_u32(fw.calls[i].params, AUX_SIZE_OFFSET), 2u);
    }
}

// A monitor of DPCD revision 1.2 exactly, with the GPU's 1.2, has its status read from the copy among its event status
// indicators
TEST(nvidia_display_link, reads_the_indicators_copy_from_dpcd_1_2_on) {
    dpcd_capabilities dpcd_1_2 = hbr3_dpcd();
    dpcd_1_2.receiver[0] = 0x12;

    link_firmware fw;
    fw.statuses[0] = {.address = 0x0200C, .bytes = {0x07, 0x00, 0x01}};

    displayport::link_status status = {};
    bool is_read = false;
    ASSERT_EQ(read_link_status(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, dpcd_1_2, true, status, is_read), OK);

    EXPECT_TRUE(is_read);
    EXPECT_TRUE(displayport::is_trained(status, 1));
    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_EQ(read_u32(fw.calls[0].params, AUX_ADDRESS_OFFSET), 0x0200Cu);
}

TEST(nvidia_display_link, reads_the_sinks_field_below_dpcd_1_2_or_without_the_gpus_1_2) {
    dpcd_capabilities dpcd_1_1 = hbr3_dpcd();
    dpcd_1_1.receiver[0] = 0x11;
    const dpcd_capabilities cases[] = {dpcd_1_1, hbr3_dpcd()};
    const bool gpu_supports_1_2[] = {true, false};
    for (uint32_t i = 0; i < 2; i++) {
        link_firmware fw;
        fw.statuses[0] = {.address = 0x00202, .bytes = {0x07, 0x00, 0x01}};

        displayport::link_status status = {};
        bool is_read = false;
        ASSERT_EQ(
            read_link_status(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, cases[i], gpu_supports_1_2[i], status, is_read),
            OK
        );

        EXPECT_TRUE(is_read);
        EXPECT_EQ(status.lanes_0_1, 0x07u);
        EXPECT_TRUE(displayport::is_trained(status, 1));
        ASSERT_EQ(fw.call_count, 1u);
        EXPECT_EQ(read_u32(fw.calls[0].params, AUX_ADDRESS_OFFSET), 0x00202u);
    }
}

// A refused status read is sent once, logged, and gives no status
TEST(nvidia_display_link, gives_no_status_when_its_read_is_refused) {
    link_firmware refusing;
    refusing.refused_command = NVIDIA_DP_AUXCH_CTRL;

    displayport::link_status status = {};
    bool is_read = true;
    s_link_log_bytes = 0;
    log::set_backend(&LINK_LOG_BACKEND);
    int32_t rc = read_link_status(refusing, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, hbr3_dpcd(), true, status, is_read);
    log::set_backend(nullptr);

    EXPECT_EQ(rc, OK);
    EXPECT_FALSE(is_read);
    EXPECT_EQ(refusing.call_count, 1u);
    EXPECT_TRUE(link_log_contains("nvidia: display common refused display id 0x100's 3-byte AUX read at 0x0200C, so "));
}
