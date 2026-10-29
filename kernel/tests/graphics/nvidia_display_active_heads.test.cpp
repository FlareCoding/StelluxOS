// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_active_heads);

using drivers::nvidia::active_head;
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

// NVIDIA's OR_GET_INFO, its parameters' size, the offsets of the fields sent in them or read from them, the output
// resource types DAC, SOR, PIOR and DSI, and NV_INVALID_OR (the index naming no output resource), restated likewise
static constexpr uint32_t NVIDIA_OR_GET_INFO       = 0x73028B;
static constexpr size_t   NVIDIA_OR_GET_INFO_BYTES = 56;
static constexpr uint32_t DISPLAY_ID_OFFSET        = 4;
static constexpr uint32_t LINK_INDEX_OFFSET        = 8;
static constexpr uint32_t LINK_TYPE_OFFSET         = 12;
static constexpr uint32_t OR_TYPE_DAC              = 1;
static constexpr uint32_t OR_TYPE_SOR              = 2;
static constexpr uint32_t OR_TYPE_PIOR             = 3;
static constexpr uint32_t OR_TYPE_DSI              = 5;
static constexpr uint32_t NO_LINK                  = 0xFFFFFFFF;

// A handle for display common unlike those `gsp` gives, and GA102's serial links, the chip table's four
static constexpr rm::handle TEST_DISPLAY_COMMON = 0x0005000B;
static constexpr uint8_t    TEST_SERIAL_LINKS   = 4;

// A link reply's index and type naming a link other than one of the chip's serial links, and the words its refusal
// logs for them
struct foreign_link {
    uint32_t    index;
    uint32_t    type;
    const char* logged;
};

// A firmware that answers GET_ACTIVE by the head a request names and OR_GET_INFO by the head showing the display id it
// names, from its fields, writing a reply only on `OK`, and records the first `MAX_CALLS` controls sent to it in order
class active_heads_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 8;
    static constexpr uint32_t MAX_BYTES = 56;

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
    bool     fills_replies = false;         // set every reply byte after the head or display id to 0xFF, then answer

    uint32_t link_index[DISPLAY_HEADS_MAX] = {}; // entry i answers the link request for head i's display id
    uint32_t link_type[DISPLAY_HEADS_MAX] = {OR_TYPE_SOR, OR_TYPE_SOR, OR_TYPE_SOR, OR_TYPE_SOR};
    uint32_t failed_link_heads = 0;              // bit i fails head i's OR_GET_INFO with `failed_link_result`
    int32_t  failed_link_result = ERR_FIRMWARE;

    // Has the firmware report `display_id` for `head` and answer its link request with `index` and `type`
    void show(uint8_t head, uint32_t display_id, uint32_t index, uint32_t type) {
        shown[head] = display_id;
        link_index[head] = index;
        link_type[head] = type;
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        record(object, command, params, size);
        bool has_params = params != nullptr;
        bool is_get_active = command == NVIDIA_GET_ACTIVE && size == NVIDIA_GET_ACTIVE_BYTES;
        bool is_or_get_info = command == NVIDIA_OR_GET_INFO && size == NVIDIA_OR_GET_INFO_BYTES;
        if (!has_params || (!is_get_active && !is_or_get_info)) {
            stray_count++;
            return OK;
        }

        uint8_t* bytes = static_cast<uint8_t*>(params);
        if (is_or_get_info) {
            return answer_link(bytes, size);
        }

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
    // The lowest head showing `display_id`, or `DISPLAY_HEADS_MAX` when no head shows it or it is 0
    uint8_t find_shown_head(uint32_t display_id) const {
        for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
            if (display_id != 0 && shown[head] == display_id) {
                return head;
            }
        }

        return DISPLAY_HEADS_MAX;
    }

    // Answers OR_GET_INFO from the link fields of the lowest head showing the display id the request names
    int32_t answer_link(uint8_t* bytes, size_t size) {
        uint32_t display_id = 0;
        string::memcpy(&display_id, bytes + DISPLAY_ID_OFFSET, sizeof(display_id));
        uint8_t head = find_shown_head(display_id);
        if (head == DISPLAY_HEADS_MAX) {
            stray_count++;
            return ERR_FIRMWARE;
        }

        if ((failed_link_heads & (1u << head)) != 0) {
            return failed_link_result;
        }

        if (fills_replies) {
            size_t answered = DISPLAY_ID_OFFSET + sizeof(uint32_t);
            string::memset(bytes + answered, 0xFF, size - answered);
        }

        string::memcpy(bytes + LINK_INDEX_OFFSET, &link_index[head], sizeof(uint32_t));
        string::memcpy(bytes + LINK_TYPE_OFFSET, &link_type[head], sizeof(uint32_t));

        return OK;
    }

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
static int32_t read_logged(active_heads_firmware& fw, uint8_t head_count, bool has_crossbar, active_heads& out) {
    s_active_heads_log_bytes = 0;
    log::set_backend(&ACTIVE_HEADS_LOG_BACKEND);
    int32_t rc = read_active_heads(fw, TEST_DISPLAY_COMMON, head_count, has_crossbar, TEST_SERIAL_LINKS, out);
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

// Expects a recorded call to send OR_GET_INFO to display common in NVIDIA's size, holding the display id and zeros in
// every other 32-bit field
static void expect_link_request(const active_heads_firmware::call& sent, uint32_t display_id) {
    EXPECT_EQ(sent.object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(sent.command, NVIDIA_OR_GET_INFO);
    EXPECT_EQ(sent.size, NVIDIA_OR_GET_INFO_BYTES);
    EXPECT_TRUE(sent.has_params);
    for (uint32_t field = 0; field < NVIDIA_OR_GET_INFO_BYTES / sizeof(uint32_t); field++) {
        uint32_t value = 0;
        string::memcpy(&value, sent.sent + field * sizeof(uint32_t), sizeof(value));
        EXPECT_EQ(value, field * sizeof(uint32_t) == DISPLAY_ID_OFFSET ? display_id : 0u);
    }
}

// Expects `state` to hold `display_id`, `index` and `type`
static void expect_head(const active_head& state, uint32_t display_id, uint32_t index, uint32_t type) {
    EXPECT_EQ(state.display_id, display_id);
    EXPECT_EQ(state.link_index, index);
    EXPECT_EQ(state.link_type, type);
}

// Heads below the count are asked their display ids in ascending order, an active head the link driving its display id
// before the next head, each request in NVIDIA's layout zeroed but for the head or display id, none past the count
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
    ASSERT_EQ(read_active_heads(earlier, TEST_DISPLAY_COMMON, 3, true, TEST_SERIAL_LINKS, earlier_heads), OK);
    ASSERT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, 3, true, TEST_SERIAL_LINKS, heads), OK);

    ASSERT_EQ(fw.call_count, 4u);
    EXPECT_EQ(fw.stray_count, 0u);
    expect_request(fw.calls[0], 0);
    expect_request(fw.calls[1], 1);
    expect_link_request(fw.calls[2], 0x100);
    expect_request(fw.calls[3], 2);
}

// With or without a crossbar, each head shows the display id and the link the firmware reports for it whatever the
// replies' other bytes hold, and a head showing none or past the count is all 0
TEST(nvidia_display_active_heads, heads_show_the_display_ids_and_links_the_firmware_reports) {
    const bool crossbars[] = {true, false};
    for (bool has_crossbar : crossbars) {
        active_heads_firmware fw;
        fw.fills_replies = true;
        fw.show(0, 0x100, 3, OR_TYPE_SOR);
        fw.show(2, 0x80000000, 0, OR_TYPE_SOR);
        fw.show(3, 0x4, 2, OR_TYPE_SOR);

        active_heads heads = {};
        fill_untouched(heads);

        ASSERT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, 3, has_crossbar, TEST_SERIAL_LINKS, heads), OK);

        EXPECT_EQ(fw.call_count, 5u);
        expect_head(heads.heads[0], 0x100, 3, OR_TYPE_SOR);
        expect_head(heads.heads[1], 0, 0, 0);
        expect_head(heads.heads[2], 0x80000000, 0, OR_TYPE_SOR);
        expect_head(heads.heads[3], 0, 0, 0);
    }
}

// A head the firmware refuses to report is logged and counts as showing nothing, and the heads after it are still read
TEST(nvidia_display_active_heads, refused_head_report_counts_that_head_as_showing_nothing) {
    active_heads_firmware fw;
    fw.shown[0] = 0x1;
    fw.shown[1] = 0x2;
    fw.failed_heads = 0x1;
    active_heads heads = {};

    ASSERT_EQ(read_logged(fw, 2, true, heads), OK);

    EXPECT_EQ(fw.call_count, 3u);
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

        EXPECT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, 3, true, TEST_SERIAL_LINKS, heads), error);

        EXPECT_EQ(fw.call_count, 3u);
        EXPECT_TRUE(is_untouched(heads));
    }
}

// An error of a link request other than `ERR_FIRMWARE` ends the read with that error after an earlier head's link was
// named, no later request sent and `out` left as it was
TEST(nvidia_display_active_heads, other_errors_of_a_link_request_end_the_read) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_BAD_MESSAGE, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        active_heads_firmware fw;
        fw.shown[0] = 0x1;
        fw.shown[1] = 0x4;
        fw.shown[2] = 0x10;
        fw.failed_link_heads = 0x2;
        fw.failed_link_result = error;

        active_heads heads = {};
        fill_untouched(heads);

        EXPECT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, 3, true, TEST_SERIAL_LINKS, heads), error);

        EXPECT_EQ(fw.call_count, 4u);
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

    EXPECT_EQ(read_logged(fw, 3, true, heads), ERR_BAD_MESSAGE);

    EXPECT_EQ(fw.call_count, 3u);
    EXPECT_TRUE(is_untouched(heads));
    EXPECT_TRUE(active_heads_log_contains(
        "nvidia: display common reports head 1 showing display ids 0x80000001, more than one"
    ));
}

// A link request the firmware refuses fails the read with `ERR_FIRMWARE` and is logged, no later request sent and
// `out` left as it was
TEST(nvidia_display_active_heads, refused_link_request_fails_the_read) {
    active_heads_firmware fw;
    fw.shown[0] = 0x1;
    fw.shown[1] = 0x100;
    fw.shown[2] = 0x4;
    fw.failed_link_heads = 0x2;

    active_heads heads = {};
    fill_untouched(heads);

    EXPECT_EQ(read_logged(fw, 3, true, heads), ERR_FIRMWARE);

    EXPECT_EQ(fw.call_count, 4u);
    EXPECT_TRUE(is_untouched(heads));
    EXPECT_TRUE(active_heads_log_contains(
        "nvidia: display common refused to name the link driving head 1's display id 0x100"
    ));
}

// With a crossbar, a link reply naming no link counts that head as showing nothing, all 0 and logged, whatever type
// the reply gives, and the heads after it are still read
TEST(nvidia_display_active_heads, linkless_head_counts_as_showing_nothing_with_a_crossbar) {
    active_heads_firmware fw;
    fw.show(0, 0x1, NO_LINK, OR_TYPE_DAC);
    fw.show(1, 0x100, 1, OR_TYPE_SOR);
    active_heads heads = {};

    ASSERT_EQ(read_logged(fw, 2, true, heads), OK);

    EXPECT_EQ(fw.call_count, 4u);
    expect_head(heads.heads[0], 0, 0, 0);
    expect_head(heads.heads[1], 0x100, 1, OR_TYPE_SOR);
    EXPECT_TRUE(active_heads_log_contains(
        "nvidia: head 0 counts as showing nothing, as display common names no link for its display id 0x1"
    ));
}

// Without a crossbar, a link reply naming no link is refused as malformed and logged after an earlier head's link was
// named, no later request sent and `out` left as it was
TEST(nvidia_display_active_heads, linkless_head_without_a_crossbar_is_refused_as_malformed) {
    active_heads_firmware fw;
    fw.show(0, 0x1, 3, OR_TYPE_SOR);
    fw.show(1, 0x100, NO_LINK, OR_TYPE_SOR);
    fw.shown[2] = 0x4;
    active_heads heads = {};
    fill_untouched(heads);

    EXPECT_EQ(read_logged(fw, 3, false, heads), ERR_BAD_MESSAGE);

    EXPECT_EQ(fw.call_count, 4u);
    EXPECT_TRUE(is_untouched(heads));
    EXPECT_TRUE(active_heads_log_contains(
        "nvidia: display common names no link for head 1's display id 0x100 and reports no crossbar"
    ));
}

// A link reply naming a DAC, PIOR or DSI, a type NVIDIA lacks or a serial link past the chip's four, even a bit from
// the no-link index or past a byte, is refused as malformed with a crossbar, logged, no later request, `out` kept
TEST(nvidia_display_active_heads, links_not_among_the_chips_serial_links_are_refused_as_malformed) {
    const foreign_link links[] = {
        {0, OR_TYPE_DAC, "names link 0 of NVIDIA's output resource type 1 for"},
        {0, OR_TYPE_PIOR, "names link 0 of NVIDIA's output resource type 3 for"},
        {0, OR_TYPE_DSI, "names link 0 of NVIDIA's output resource type 5 for"},
        {0, 0x10002, "names link 0 of NVIDIA's output resource type 65538 for"},
        {TEST_SERIAL_LINKS, OR_TYPE_SOR, "names link 4 of NVIDIA's output resource type 2 for"},
        {0xFFFFFFFE, OR_TYPE_SOR, "names link 4294967294 of NVIDIA's output resource type 2 for"},
        {0x7FFFFFFF, OR_TYPE_SOR, "names link 2147483647 of NVIDIA's output resource type 2 for"},
        {0x100, OR_TYPE_SOR, "names link 256 of NVIDIA's output resource type 2 for"},
    };

    for (const foreign_link& link : links) {
        active_heads_firmware fw;
        fw.show(0, 0x1, 2, OR_TYPE_SOR);
        fw.show(1, 0x100, link.index, link.type);
        fw.shown[2] = 0x4;
        active_heads heads = {};
        fill_untouched(heads);

        EXPECT_EQ(read_logged(fw, 3, true, heads), ERR_BAD_MESSAGE);

        EXPECT_EQ(fw.call_count, 4u);
        EXPECT_TRUE(is_untouched(heads));
        EXPECT_TRUE(active_heads_log_contains(link.logged));
        EXPECT_TRUE(active_heads_log_contains("for head 1's display id 0x100, not one of the chip's 4 serial links"));
    }
}

// With `DISPLAY_HEADS_MAX` heads, the most NVIDIA's driver allows on one GPU, every head can show a display id through
// its link
TEST(nvidia_display_active_heads, every_head_can_show_a_display_id) {
    active_heads_firmware fw;
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        fw.show(head, 1u << (head * 8), head, OR_TYPE_SOR);
    }

    active_heads heads = {};

    ASSERT_EQ(read_active_heads(fw, TEST_DISPLAY_COMMON, DISPLAY_HEADS_MAX, true, TEST_SERIAL_LINKS, heads), OK);

    EXPECT_EQ(fw.call_count, 2u * DISPLAY_HEADS_MAX);
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        expect_head(heads.heads[head], 1u << (head * 8), head, OR_TYPE_SOR);
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
