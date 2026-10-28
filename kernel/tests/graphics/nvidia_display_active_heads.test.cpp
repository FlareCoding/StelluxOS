// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_active_heads);

using drivers::nvidia::active_heads;
using drivers::nvidia::DISPLAY_HEADS_MAX;
using drivers::nvidia::display_outputs;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::find_boot_head;
using drivers::nvidia::find_shown_output;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::read_active_heads;
namespace rm = drivers::nvidia::rm;

// NVIDIA's GET_ACTIVE, its parameters' size and the offsets of the fields sent in them or read from them, restated so
// the tests hold the requests to NVIDIA's layout
static constexpr uint32_t NVIDIA_GET_ACTIVE       = 0x730126;
static constexpr size_t   NVIDIA_GET_ACTIVE_BYTES = 16;
static constexpr uint32_t HEAD_OFFSET             = 4;
static constexpr uint32_t SHOWN_DISPLAY_ID_OFFSET = 12;

// A handle for display common unlike those `gsp` gives
static constexpr rm::handle TEST_DISPLAY_COMMON = 0x0005000B;

// A firmware that answers GET_ACTIVE from its fields by the head a request names, writing a reply only on `OK`, and
// records the first `MAX_CALLS` controls sent to it in order
class active_heads_firmware : public firmware {
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

    uint32_t shown[DISPLAY_HEADS_MAX] = {}; // entry i is the display id a successful request for head i reports
    uint32_t failed_heads = 0;              // heads, bit i for head i, whose GET_ACTIVE returns `failed_result`
    int32_t  failed_result = ERR_FIRMWARE;
    bool     fills_replies = false;         // set every reply byte after the head to 0xFF, then write the display id

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        record(object, command, params, size);
        bool is_get_active = command == NVIDIA_GET_ACTIVE && size == NVIDIA_GET_ACTIVE_BYTES;
        if (params == nullptr || !is_get_active) {
            stray_count++;
            return OK;
        }

        uint8_t* bytes = static_cast<uint8_t*>(params);
        uint32_t head = 0;
        string::memcpy(&head, bytes + HEAD_OFFSET, sizeof(head));
        if (head >= DISPLAY_HEADS_MAX) {
            stray_count++;
            return ERR_FIRMWARE;
        }

        if ((failed_heads & (1u << head)) != 0) {
            return failed_result;
        }

        if (fills_replies) {
            string::memset(bytes + HEAD_OFFSET + sizeof(uint32_t), 0xFF, size - HEAD_OFFSET - sizeof(uint32_t));
        }

        string::memcpy(bytes + SHOWN_DISPLAY_ID_OFFSET, &shown[head], sizeof(uint32_t));

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
};

// The byte every byte of an `out` holds before a call, so a test sees whether the call wrote it
static constexpr uint8_t UNTOUCHED = 0xEE;

// Sets every byte of `out` to `UNTOUCHED`
static void fill_untouched(active_heads& out) {
    string::memset(&out, UNTOUCHED, sizeof(out));
}

// Whether every byte of `out` still holds `UNTOUCHED`
static bool is_untouched(const active_heads& out) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&out);
    for (size_t i = 0; i < sizeof(out); i++) {
        if (bytes[i] != UNTOUCHED) {
            return false;
        }
    }

    return true;
}

// The text logged while `ACTIVE_HEADS_LOG_BACKEND` is set
static char     s_active_heads_log[2048] = {};
static uint32_t s_active_heads_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_active_heads_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_active_heads_log_bytes < sizeof(s_active_heads_log); i++) {
        s_active_heads_log[s_active_heads_log_bytes++] = data[i];
    }
}

static constexpr log::backend ACTIVE_HEADS_LOG_BACKEND = {.write = keep_active_heads_log};

// Whether the kept text holds `text` anywhere
static bool active_heads_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_active_heads_log_bytes; start++) {
        if (string::memcmp(s_active_heads_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// Reads the active heads with the log kept
static int32_t read_logged(active_heads_firmware& fw, uint8_t head_count, active_heads& out) {
    s_active_heads_log_bytes = 0;
    log::set_backend(&ACTIVE_HEADS_LOG_BACKEND);
    int32_t rc = read_active_heads(fw, TEST_DISPLAY_COMMON, head_count, out);
    log::set_backend(nullptr);

    return rc;
}

// Expects a recorded call to send GET_ACTIVE to display common in NVIDIA's size, holding the head and zeros in every
// other 32-bit field, so its flags are 0 (NVIDIA's CLIENT_DISABLE)
static void expect_request(const active_heads_firmware::call& sent, uint32_t head) {
    EXPECT_EQ(sent.object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(sent.command, NVIDIA_GET_ACTIVE);
    EXPECT_EQ(sent.size, NVIDIA_GET_ACTIVE_BYTES);
    EXPECT_TRUE(sent.has_params);
    for (uint32_t field = 0; field < NVIDIA_GET_ACTIVE_BYTES / sizeof(uint32_t); field++) {
        uint32_t value = 0;
        string::memcpy(&value, sent.sent + field * sizeof(uint32_t), sizeof(value));
        EXPECT_EQ(value, field * sizeof(uint32_t) == HEAD_OFFSET ? head : 0u);
    }
}

// Each head below the count is asked its display id in ascending order, in NVIDIA's layout, each request zeroed but for
// the head, and no head past the count is asked
TEST(nvidia_display_active_heads, requests_go_to_display_common_head_by_head_in_nvidias_layout) {
    active_heads_firmware earlier;
    earlier.fills_replies = true;
    earlier.shown[1] = 0x100;
    active_heads earlier_heads = {};

    active_heads_firmware fw;
    fw.fills_replies = true;
    fw.shown[1] = 0x100;
    fw.shown[3] = 0x1;
    active_heads heads = {};

    // The earlier read fills the stack the next read's requests are expected to reuse, so a request not zeroed would
    // likely carry its replies
    ASSERT_EQ(read_active_heads(earlier, TEST_DISPLAY_COMMON, 3, earlier_heads), OK);
    ASSERT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, 3, heads), OK);

    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_EQ(fw.stray_count, 0u);
    expect_request(fw.calls[0], 0);
    expect_request(fw.calls[1], 1);
    expect_request(fw.calls[2], 2);
}

// Each head shows the display id the firmware reports for it, whatever the reply's other bytes hold, a head reported
// showing none shows nothing, and every head past the count shows nothing
TEST(nvidia_display_active_heads, heads_show_the_display_ids_the_firmware_reports) {
    active_heads_firmware fw;
    fw.fills_replies = true;
    fw.shown[0] = 0x100;
    fw.shown[2] = 0x80000000;
    fw.shown[3] = 0x4;

    active_heads heads = {};
    fill_untouched(heads);

    ASSERT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, 3, heads), OK);

    EXPECT_EQ(fw.call_count, 3u);
    EXPECT_EQ(heads.heads[0].display_id, 0x100u);
    EXPECT_EQ(heads.heads[1].display_id, 0u);
    EXPECT_EQ(heads.heads[2].display_id, 0x80000000u);
    EXPECT_EQ(heads.heads[3].display_id, 0u);
}

// A head the firmware refuses to report is logged and counts as showing nothing, and the heads after it are still read
TEST(nvidia_display_active_heads, refused_head_report_counts_that_head_as_showing_nothing) {
    active_heads_firmware fw;
    fw.shown[0] = 0x1;
    fw.shown[1] = 0x2;
    fw.failed_heads = 0x1;
    active_heads heads = {};

    ASSERT_EQ(read_logged(fw, 2, heads), OK);

    EXPECT_EQ(fw.call_count, 2u);
    EXPECT_EQ(heads.heads[0].display_id, 0u);
    EXPECT_EQ(heads.heads[1].display_id, 0x2u);
    EXPECT_TRUE(active_heads_log_contains(
        "nvidia: head 0 counts as showing nothing, as display common refused to report it"
    ));
}

// Any other error of a head's report ends the read with that error after an earlier head showed a display id, no later
// request sent and `out` left as it was
TEST(nvidia_display_active_heads, other_errors_of_a_head_report_end_the_read) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_BAD_MESSAGE, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        active_heads_firmware fw;
        fw.shown[0] = 0x1;
        fw.shown[2] = 0x4;
        fw.failed_heads = 0x2;
        fw.failed_result = error;

        active_heads heads = {};
        fill_untouched(heads);

        EXPECT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, 3, heads), error);

        EXPECT_EQ(fw.call_count, 2u);
        EXPECT_TRUE(is_untouched(heads));
    }
}

// A head reported showing several display ids, even ones whose low half alone is a single display id, is refused as
// malformed and logged, no later request sent and `out` left as it was
TEST(nvidia_display_active_heads, head_showing_several_display_ids_is_refused_as_malformed) {
    active_heads_firmware fw;
    fw.shown[0] = 0x1;
    fw.shown[1] = 0x80000001;
    fw.shown[2] = 0x4;
    active_heads heads = {};
    fill_untouched(heads);

    EXPECT_EQ(read_logged(fw, 3, heads), ERR_BAD_MESSAGE);

    EXPECT_EQ(fw.call_count, 2u);
    EXPECT_TRUE(is_untouched(heads));
    EXPECT_TRUE(active_heads_log_contains(
        "nvidia: display common reports head 1 showing display ids 0x80000001, more than one"
    ));
}

// With `DISPLAY_HEADS_MAX` heads, the most NVIDIA's driver allows on one GPU, every head can show a display id
TEST(nvidia_display_active_heads, every_head_can_show_a_display_id) {
    active_heads_firmware fw;
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        fw.shown[head] = 1u << (head * 8);
    }

    active_heads heads = {};

    ASSERT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, DISPLAY_HEADS_MAX, heads), OK);

    EXPECT_EQ(fw.call_count, static_cast<uint32_t>(DISPLAY_HEADS_MAX));
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        EXPECT_EQ(heads.heads[head].display_id, 1u << (head * 8));
    }
}

// The boot head is the lowest active head, from the last head to the first, whichever heads after it are active, and
// there is none with no head active
TEST(nvidia_display_active_heads, boot_head_is_the_lowest_active_head) {
    active_heads heads = {};
    heads.heads[DISPLAY_HEADS_MAX - 1].display_id = 0x1;

    uint8_t boot_head = 0xEE;
    ASSERT_TRUE(find_boot_head(heads, boot_head));
    EXPECT_EQ(boot_head, DISPLAY_HEADS_MAX - 1u);

    heads.heads[1].display_id = 0x4;
    ASSERT_TRUE(find_boot_head(heads, boot_head));
    EXPECT_EQ(boot_head, 1u);

    heads.heads[0].display_id = 0x8;
    ASSERT_TRUE(find_boot_head(heads, boot_head));
    EXPECT_EQ(boot_head, 0u);

    const active_heads none = {};
    boot_head = 0xEE;
    EXPECT_FALSE(find_boot_head(none, boot_head));
    EXPECT_EQ(boot_head, 0xEEu);
}

// The output a head shows is the one whose display id is the head's, the last output as well as the first, and there is
// none for a display id that is no output's, the entry past the outputs' count included, or for a head showing nothing
TEST(nvidia_display_active_heads, shown_output_is_the_one_whose_display_id_the_head_shows) {
    display_outputs outputs = {};
    outputs.count = 3;
    outputs.outputs[0].display_id = 0x1;
    outputs.outputs[1].display_id = 0x4;
    outputs.outputs[2].display_id = 0x100;
    outputs.outputs[3].display_id = 0x40;

    active_heads heads = {};
    heads.heads[0].display_id = 0x2;
    heads.heads[1].display_id = 0x100;
    heads.heads[3].display_id = 0x1;

    uint8_t output = 0xEE;
    ASSERT_TRUE(find_shown_output(heads, 1, outputs, output));
    EXPECT_EQ(output, 2u);
    ASSERT_TRUE(find_shown_output(heads, 3, outputs, output));
    EXPECT_EQ(output, 0u);

    output = 0xEE;
    EXPECT_FALSE(find_shown_output(heads, 0, outputs, output));
    EXPECT_FALSE(find_shown_output(heads, 2, outputs, output));
    EXPECT_EQ(output, 0xEEu);

    heads.heads[0].display_id = 0x40;
    EXPECT_FALSE(find_shown_output(heads, 0, outputs, output));
    EXPECT_EQ(output, 0xEEu);
}
