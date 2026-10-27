// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_outputs);

using drivers::nvidia::DISPLAY_IDS_MAX;
using drivers::nvidia::display_ids;
using drivers::nvidia::display_output;
using drivers::nvidia::display_outputs;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::read_display_outputs;
namespace rm = drivers::nvidia::rm;

// NVIDIA's DFP_GET_INFO, its parameters' size and the offsets of the fields read from or sent in them, restated so the
// tests hold the requests to NVIDIA's layout
static constexpr uint32_t NVIDIA_DFP_GET_INFO       = 0x731140;
static constexpr size_t   NVIDIA_DFP_GET_INFO_BYTES = 16;
static constexpr uint32_t FLAGS_OFFSET              = 8;
static constexpr uint32_t DISPLAY_ID_OFFSET         = 4;

// DFP_GET_INFO's flags the tests build, from ctrl0073dfp.h
static constexpr uint32_t SIGNAL_TMDS        = 0;
static constexpr uint32_t SIGNAL_LVDS        = 1;
static constexpr uint32_t SIGNAL_SDI         = 2;
static constexpr uint32_t SIGNAL_DISPLAYPORT = 3;
static constexpr uint32_t SIGNAL_DSI         = 4;
static constexpr uint32_t SIGNAL_WRBK        = 5;
static constexpr uint32_t LANE_QUAD          = 3u << 3;
static constexpr uint32_t LIMIT_60HZ_RR      = 1u << 6;
static constexpr uint32_t HDMI_CAPABLE       = 1u << 8;
static constexpr uint32_t PREFER_RBR         = 1u << 16;
static constexpr uint32_t DP_LINK_BW_8_10    = 4u << 17;
static constexpr uint32_t LINK_DUAL          = 2u << 20;
static constexpr uint32_t PHY_REPEATERS_15   = 15u << 26;

// A handle for display common unlike those `gsp` gives
static constexpr rm::handle TEST_DISPLAY_COMMON = 0x0005000B;

// A firmware that answers DFP_GET_INFO from its fields by the display id a request names, writing a reply only on `OK`,
// and records the first `MAX_CALLS` controls sent to it in order
class outputs_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 8;
    static constexpr uint32_t MAX_BYTES = 16;

    struct call {
        rm::handle object;
        uint32_t   command;
        size_t     size;
        bool       has_params;
        uint8_t    sent[MAX_BYTES]; // the parameters' first bytes
    };

    call     calls[MAX_CALLS] = {};
    uint32_t call_count = 0;
    uint32_t stray_count = 0;

    uint32_t flags[DISPLAY_IDS_MAX] = {}; // entry i is the flags a successful request for display id 1 << i reports
    uint32_t facts_failed_ids = 0; // display ids whose DFP_GET_INFO returns `facts_result`
    int32_t  facts_result = ERR_FIRMWARE;
    bool     fills_replies = false; // set every reply byte after the display id to 0xFF before the fields it answers

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        record(object, command, params, size);
        if (params == nullptr || size < DISPLAY_ID_OFFSET + sizeof(uint32_t)) {
            stray_count++;
            return OK;
        }

        uint8_t* bytes = static_cast<uint8_t*>(params);
        uint32_t display_id = 0;
        string::memcpy(&display_id, bytes + DISPLAY_ID_OFFSET, sizeof(display_id));
        bool is_single_bit = display_id != 0 && (display_id & (display_id - 1)) == 0;
        if (!is_single_bit) {
            stray_count++;
            return ERR_FIRMWARE;
        }

        if (command == NVIDIA_DFP_GET_INFO && size == NVIDIA_DFP_GET_INFO_BYTES) {
            if ((facts_failed_ids & display_id) != 0) {
                return facts_result;
            }

            fill(bytes, size);
            uint32_t bit = static_cast<uint32_t>(__builtin_ctz(display_id));
            string::memcpy(bytes + FLAGS_OFFSET, &flags[bit], sizeof(uint32_t));
            return OK;
        }

        stray_count++;

        return OK;
    }

    // No function under test allocates, releases, sends an internal control or takes a handle, so each is a stray
    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override {
        stray_count++;
        return OK;
    }

    int32_t release(rm::handle) override {
        stray_count++;
        return OK;
    }

    int32_t internal_control(uint32_t, void*, size_t) override {
        stray_count++;
        return OK;
    }

    rm::handle new_handle() override {
        stray_count++;
        return 0;
    }

    rm::handle client() const override {
        return 0;
    }

private:
    void record(rm::handle object, uint32_t command, void* params, size_t size) {
        call_count++;
        if (call_count > MAX_CALLS) {
            return;
        }

        call& recorded = calls[call_count - 1];
        recorded.object = object;
        recorded.command = command;
        recorded.size = size;
        recorded.has_params = params != nullptr;
        if (params != nullptr) {
            string::memcpy(recorded.sent, params, size < MAX_BYTES ? size : MAX_BYTES);
        }
    }

    // Sets every reply byte after the display id to 0xFF when `fills_replies` is set
    void fill(uint8_t* bytes, size_t size) const {
        if (!fills_replies) {
            return;
        }

        string::memset(bytes + DISPLAY_ID_OFFSET + sizeof(uint32_t), 0xFF, size - DISPLAY_ID_OFFSET - sizeof(uint32_t));
    }
};

// The byte every byte of an `out` holds before a call, so a test sees whether the call wrote it
static constexpr uint8_t UNTOUCHED = 0xEE;

// Sets every byte of `out` to `UNTOUCHED`
static void fill_untouched(display_outputs& out) {
    string::memset(&out, UNTOUCHED, sizeof(out));
}

// Whether every byte of `out` still holds `UNTOUCHED`
static bool is_untouched(const display_outputs& out) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&out);
    for (size_t i = 0; i < sizeof(out); i++) {
        if (bytes[i] != UNTOUCHED) {
            return false;
        }
    }

    return true;
}

// The text logged while `OUTPUTS_LOG_BACKEND` is set
static char     s_outputs_log[2048] = {};
static uint32_t s_outputs_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_outputs_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_outputs_log_bytes < sizeof(s_outputs_log); i++) {
        s_outputs_log[s_outputs_log_bytes++] = data[i];
    }
}

static constexpr log::backend OUTPUTS_LOG_BACKEND = {.write = keep_outputs_log};

// Whether the kept text holds `text` anywhere
static bool outputs_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_outputs_log_bytes; start++) {
        if (string::memcmp(s_outputs_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// Reads the outputs with the log kept
static int32_t read_logged(outputs_firmware& fw, const display_ids& ids, display_outputs& out) {
    s_outputs_log_bytes = 0;
    log::set_backend(&OUTPUTS_LOG_BACKEND);
    int32_t rc = read_display_outputs(fw, TEST_DISPLAY_COMMON, ids, out);
    log::set_backend(nullptr);

    return rc;
}

// Expects a recorded call to send DFP_GET_INFO to display common in NVIDIA's size, holding the display id and zeros in
// every other 32-bit field
static void expect_request(const outputs_firmware::call& sent, uint32_t display_id) {
    EXPECT_EQ(sent.object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(sent.command, NVIDIA_DFP_GET_INFO);
    EXPECT_EQ(sent.size, NVIDIA_DFP_GET_INFO_BYTES);
    EXPECT_TRUE(sent.has_params);
    for (uint32_t field = 0; field < NVIDIA_DFP_GET_INFO_BYTES / sizeof(uint32_t); field++) {
        uint32_t value = 0;
        string::memcpy(&value, sent.sent + field * sizeof(uint32_t), sizeof(value));
        EXPECT_EQ(value, field * sizeof(uint32_t) == DISPLAY_ID_OFFSET ? display_id : 0u);
    }
}

// Each static digital display id is asked its digital facts in ascending order of its bit, in NVIDIA's layout, each
// request zeroed but for the display id, and no other display id is asked about
TEST(nvidia_display_outputs, requests_go_to_display_common_in_order_and_nvidias_layout) {
    const display_ids ids = {.supported = 0xF, .connected = 0x4, .digital = 0x7, .static_digital = 0x5};
    outputs_firmware earlier;
    earlier.fills_replies = true;
    earlier.flags[0] = SIGNAL_DISPLAYPORT;
    earlier.flags[2] = SIGNAL_DISPLAYPORT;
    display_outputs earlier_outputs = {};

    outputs_firmware fw;
    fw.fills_replies = true;
    fw.flags[0] = SIGNAL_DISPLAYPORT;
    fw.flags[2] = SIGNAL_DISPLAYPORT;
    display_outputs outputs = {};

    // The earlier read fills the stack the next read's requests are expected to reuse, so a request not zeroed would
    // likely carry its replies
    ASSERT_EQ(read_display_outputs(earlier, TEST_DISPLAY_COMMON, ids, earlier_outputs), OK);
    ASSERT_EQ(read_display_outputs(fw, TEST_DISPLAY_COMMON, ids, outputs), OK);

    ASSERT_EQ(fw.call_count, 2u);
    EXPECT_EQ(fw.stray_count, 0u);
    expect_request(fw.calls[0], 0x1);
    expect_request(fw.calls[1], 0x4);
    EXPECT_EQ(outputs.count, 2u);
}

// The static digital display ids whose signal is DisplayPort are the outputs, numbered from 0 in ascending order of
// their bit up to bit 31, each marked connected when the display ids say so, whatever the reply's other bytes hold
TEST(nvidia_display_outputs, static_displayport_ids_are_outputs_in_ascending_bit_order) {
    const display_ids ids = {
        .supported = 0x80000F03,
        .connected = 0x00000903,
        .digital = 0x80000F01,
        .static_digital = 0x80000F00,
    };

    outputs_firmware fw;
    fw.fills_replies = true;

    fw.flags[0] = SIGNAL_DISPLAYPORT;
    fw.flags[8] = SIGNAL_DISPLAYPORT;
    fw.flags[9] = SIGNAL_DISPLAYPORT;
    fw.flags[10] = SIGNAL_TMDS | HDMI_CAPABLE;
    fw.flags[11] = SIGNAL_DISPLAYPORT;
    fw.flags[31] = SIGNAL_DISPLAYPORT;

    display_outputs outputs = {};
    fill_untouched(outputs);

    ASSERT_EQ(read_display_outputs(fw, TEST_DISPLAY_COMMON, ids, outputs), OK);

    EXPECT_EQ(fw.call_count, 5u);
    ASSERT_EQ(outputs.count, 4u);
    const uint32_t output_ids[] = {0x100, 0x200, 0x800, 0x80000000};
    const bool connected[] = {true, false, true, false};
    for (uint32_t index = 0; index < 4; index++) {
        EXPECT_EQ(outputs.outputs[index].display_id, output_ids[index]);
        EXPECT_EQ(outputs.outputs[index].is_connected, connected[index]);
    }
}

// An output's lanes and link rate, kept as NVIDIA's codes, and its 60 Hz limit and RBR request come from their own bits
// of the digital facts, whatever the other bits hold
TEST(nvidia_display_outputs, digital_facts_come_from_their_own_bits) {
    const display_ids ids = {.supported = 0x7, .connected = 0, .digital = 0x7, .static_digital = 0x7};
    outputs_firmware fw;

    uint32_t others = HDMI_CAPABLE | LINK_DUAL | PHY_REPEATERS_15;
    fw.flags[0] = SIGNAL_DISPLAYPORT | LANE_QUAD | LIMIT_60HZ_RR | PREFER_RBR | DP_LINK_BW_8_10 | others;
    fw.flags[1] = SIGNAL_DISPLAYPORT | 0x38 | PREFER_RBR | 0xE0000;
    fw.flags[2] = SIGNAL_DISPLAYPORT | 0xFFF0FF80;
    display_outputs outputs = {};

    ASSERT_EQ(read_display_outputs(fw, TEST_DISPLAY_COMMON, ids, outputs), OK);

    ASSERT_EQ(outputs.count, 3u);

    const display_output& full = outputs.outputs[0];
    EXPECT_EQ(full.lanes_code, 3u);
    EXPECT_EQ(full.link_rate_code, 4u);
    EXPECT_TRUE(full.is_limited_to_60_hz);
    EXPECT_TRUE(full.prefers_rbr);
    EXPECT_FALSE(full.is_connected);

    const display_output& highest_codes = outputs.outputs[1];
    EXPECT_EQ(highest_codes.lanes_code, 7u);
    EXPECT_EQ(highest_codes.link_rate_code, 7u);
    EXPECT_FALSE(highest_codes.is_limited_to_60_hz);
    EXPECT_TRUE(highest_codes.prefers_rbr);

    const display_output& other_bits = outputs.outputs[2];
    EXPECT_EQ(other_bits.lanes_code, 0u);
    EXPECT_EQ(other_bits.link_rate_code, 0u);
    EXPECT_FALSE(other_bits.is_limited_to_60_hz);
    EXPECT_FALSE(other_bits.prefers_rbr);
}

// A static digital display id whose signal is not DisplayPort is logged and is not an output, whatever its signal,
// even one equal to DisplayPort in its low two bits
TEST(nvidia_display_outputs, display_ids_not_displayport_are_logged_and_not_outputs) {
    const display_ids ids = {.supported = 0x7F, .connected = 0x7F, .digital = 0x7F, .static_digital = 0x7F};
    outputs_firmware fw;
    display_outputs outputs = {};

    fw.flags[0] = SIGNAL_TMDS | HDMI_CAPABLE;
    fw.flags[1] = SIGNAL_LVDS;
    fw.flags[2] = SIGNAL_SDI;
    fw.flags[3] = SIGNAL_DSI;
    fw.flags[4] = SIGNAL_WRBK;

    fw.flags[5] = 7;
    fw.flags[6] = SIGNAL_DISPLAYPORT | LANE_QUAD;

    ASSERT_EQ(read_logged(fw, ids, outputs), OK);

    ASSERT_EQ(outputs.count, 1u);
    EXPECT_EQ(outputs.outputs[0].display_id, 0x40u);
    EXPECT_EQ(fw.call_count, 7u);
    const char* logged[] = {
        "nvidia: display id 0x1 is not an output, as display common reports its signal 0, not DisplayPort's 3",
        "nvidia: display id 0x2 is not an output, as display common reports its signal 1, not DisplayPort's 3",
        "nvidia: display id 0x4 is not an output, as display common reports its signal 2, not DisplayPort's 3",
        "nvidia: display id 0x8 is not an output, as display common reports its signal 4, not DisplayPort's 3",
        "nvidia: display id 0x10 is not an output, as display common reports its signal 5, not DisplayPort's 3",
        "nvidia: display id 0x20 is not an output, as display common reports its signal 7, not DisplayPort's 3",
    };

    for (const char* line : logged) {
        EXPECT_TRUE(outputs_log_contains(line));
    }
}

// A display id whose digital facts the firmware refuses is logged and is not an output, and the display ids after it
// still become outputs
TEST(nvidia_display_outputs, refused_digital_facts_leave_only_that_display_id_out_of_the_outputs) {
    const display_ids ids = {.supported = 0x7, .connected = 0x7, .digital = 0x7, .static_digital = 0x7};
    outputs_firmware fw;
    fw.facts_failed_ids = 0x2;
    display_outputs outputs = {};

    fw.flags[0] = SIGNAL_DISPLAYPORT;
    fw.flags[1] = SIGNAL_DISPLAYPORT;
    fw.flags[2] = SIGNAL_DISPLAYPORT;

    ASSERT_EQ(read_logged(fw, ids, outputs), OK);

    EXPECT_EQ(fw.call_count, 3u);
    ASSERT_EQ(outputs.count, 2u);
    EXPECT_EQ(outputs.outputs[0].display_id, 0x1u);
    EXPECT_EQ(outputs.outputs[1].display_id, 0x4u);
    EXPECT_TRUE(outputs.outputs[1].is_connected);
    EXPECT_TRUE(outputs_log_contains(
        "nvidia: display id 0x2 is not an output, as display common refused its digital facts"
    ));
}

// Any other error of a digital facts request ends the read with that error after an earlier display id became an
// output, no later request sent and `out` left as it was
TEST(nvidia_display_outputs, other_errors_of_a_digital_facts_request_end_the_read) {
    const display_ids ids = {.supported = 0x7, .connected = 0x7, .digital = 0x7, .static_digital = 0x7};
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_BAD_MESSAGE, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        outputs_firmware fw;
        fw.facts_failed_ids = 0x2;
        fw.facts_result = error;

        fw.flags[0] = SIGNAL_DISPLAYPORT;
        fw.flags[1] = SIGNAL_DISPLAYPORT;
        fw.flags[2] = SIGNAL_DISPLAYPORT;

        display_outputs outputs = {};
        fill_untouched(outputs);

        EXPECT_EQ(read_display_outputs(fw, TEST_DISPLAY_COMMON, ids, outputs), error);

        EXPECT_EQ(fw.call_count, 2u);
        EXPECT_TRUE(is_untouched(outputs));
    }
}

// With no static digital display id the read asks nothing and gives no output
TEST(nvidia_display_outputs, no_static_digital_display_id_gives_no_output) {
    const display_ids ids = {.supported = 0xFF, .connected = 0xFF, .digital = 0xFF, .static_digital = 0};
    outputs_firmware fw;
    display_outputs outputs = {};
    fill_untouched(outputs);

    ASSERT_EQ(read_display_outputs(fw, TEST_DISPLAY_COMMON, ids, outputs), OK);

    EXPECT_EQ(fw.call_count, 0u);
    EXPECT_EQ(outputs.count, 0u);
}

// All 32 display ids can be outputs, filling every entry
TEST(nvidia_display_outputs, every_display_id_can_be_an_output) {
    const display_ids ids = {
        .supported = 0xFFFFFFFF,
        .connected = 0xFFFFFFFF,
        .digital = 0xFFFFFFFF,
        .static_digital = 0xFFFFFFFF,
    };

    outputs_firmware fw;
    for (uint32_t bit = 0; bit < DISPLAY_IDS_MAX; bit++) {
        fw.flags[bit] = SIGNAL_DISPLAYPORT;
    }

    display_outputs outputs = {};

    ASSERT_EQ(read_display_outputs(fw, TEST_DISPLAY_COMMON, ids, outputs), OK);

    EXPECT_EQ(fw.call_count, DISPLAY_IDS_MAX);
    ASSERT_EQ(outputs.count, DISPLAY_IDS_MAX);
    for (uint32_t index = 0; index < DISPLAY_IDS_MAX; index++) {
        EXPECT_EQ(outputs.outputs[index].display_id, 1u << index);
        EXPECT_TRUE(outputs.outputs[index].is_connected);
    }
}
