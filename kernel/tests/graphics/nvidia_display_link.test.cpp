// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_link.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_link);

using drivers::nvidia::assign_output_link;
using drivers::nvidia::chip_displayport;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::output_link;
using drivers::nvidia::read_link_capabilities;
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

// A firmware that records each control's command and parameters as sent, refuses the first `refusals` requests of
// `refused_command`, and answers the assignment, output resource facts and capabilities from its fields
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
        }
    }
};

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
