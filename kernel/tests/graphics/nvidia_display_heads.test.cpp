// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_heads);

using drivers::nvidia::chip_display;
using drivers::nvidia::display_heads;
using drivers::nvidia::DISPLAY_HEADS_MAX;
using drivers::nvidia::DISPLAY_WINDOWS_MAX;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::NO_HEAD;
using drivers::nvidia::OK;
using drivers::nvidia::read_display_heads;
using drivers::nvidia::windows_of_head;
namespace rm = drivers::nvidia::rm;

// NVIDIA's head controls, their parameters' sizes, the offsets read from their replies, the windows the last gives
// masks for and window 0's mask in a flexible mapping, restated so the tests hold the requests to NVIDIA's layouts
static constexpr uint32_t NVIDIA_GET_NUM_HEADS           = 0x730102;
static constexpr size_t   NVIDIA_GET_NUM_HEADS_BYTES     = 12;
static constexpr uint32_t HEAD_COUNT_OFFSET              = 8;
static constexpr uint32_t NVIDIA_GET_ALL_HEAD_MASK       = 0x730287;
static constexpr size_t   NVIDIA_GET_ALL_HEAD_MASK_BYTES = 8;
static constexpr uint32_t HEAD_MASK_OFFSET               = 4;
static constexpr uint32_t NVIDIA_GET_WINDOW_HEADS        = 0x7302AD;
static constexpr size_t   NVIDIA_GET_WINDOW_HEADS_BYTES  = 36;
static constexpr uint32_t WINDOW_MASKS_OFFSET            = 4;
static constexpr uint32_t NVIDIA_MAX_WINDOWS             = 32;
static constexpr uint8_t  NVIDIA_FLEXIBLE_MASK           = 0xFF;

// NVIDIA's NV_MAX_HEADS, the most heads its driver allows on one GPU
static constexpr uint8_t NVIDIA_MAX_HEADS = 4;

// A handle for display common unlike those `gsp` gives, and a chip of GA102's 4 heads and 8 windows, with the classes
// left 0, as reading the heads never uses them
static constexpr rm::handle   TEST_DISPLAY_COMMON = 0x0005000B;
static constexpr chip_display TEST_DISPLAY        = {
    .head_count         = 4,
    .window_count       = 8,
    .display_class      = 0,
    .core_channel_class = 0,
};

// A firmware that records the controls sent to it in order and answers each with its own entry of `results`, and `OK`
// past them, writing a call's entry of `replies` into its parameters on `OK`
class heads_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 4;
    static constexpr uint32_t MAX_BYTES = NVIDIA_GET_WINDOW_HEADS_BYTES;

    struct call {
        rm::handle object;
        uint32_t   command;
        size_t     size;
        bool       has_params;
        uint8_t    sent[MAX_BYTES]; // the parameters' first bytes
    };

    call     calls[MAX_CALLS] = {};
    int32_t  results[MAX_CALLS] = {};
    uint8_t  replies[MAX_CALLS][MAX_BYTES] = {};
    uint32_t call_count = 0;
    uint32_t stray_count = 0;

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        call_count++;
        if (call_count > MAX_CALLS) {
            return OK;
        }

        uint32_t index = call_count - 1;
        size_t copied = size < MAX_BYTES ? size : MAX_BYTES;
        bool has_params = params != nullptr;
        calls[index] = {.object = object, .command = command, .size = size, .has_params = has_params, .sent = {}};
        if (has_params) {
            string::memcpy(calls[index].sent, params, copied);
        }

        if (results[index] == OK && has_params) {
            string::memcpy(params, replies[index], copied);
        }

        return results[index];
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
};

// A report of the heads and windows as display common's three replies give it
struct head_report {
    uint32_t head_count;
    uint32_t head_mask;
    uint8_t  window_masks[NVIDIA_MAX_WINDOWS];
};

// The RTX 3080's report as its firmware gives it: 4 heads, all usable, and its 8 windows flexible, the rest 0
static constexpr head_report RTX_3080_REPORT = {
    .head_count = 4,
    .head_mask = 0xF,
    .window_masks = {
        NVIDIA_FLEXIBLE_MASK,
        NVIDIA_FLEXIBLE_MASK,
        NVIDIA_FLEXIBLE_MASK,
        NVIDIA_FLEXIBLE_MASK,
        NVIDIA_FLEXIBLE_MASK,
        NVIDIA_FLEXIBLE_MASK,
        NVIDIA_FLEXIBLE_MASK,
        NVIDIA_FLEXIBLE_MASK,
    },
};

// Heads each `out` holds before a call, so a test sees whether the call wrote it
static constexpr display_heads UNTOUCHED = {.count = 0xEE, .window_head = {0xEE, 0xEE, 0xEE}};

// The text logged while `HEADS_LOG_BACKEND` is set
static char     s_heads_log[512] = {};
static uint32_t s_heads_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_heads_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_heads_log_bytes < sizeof(s_heads_log); i++) {
        s_heads_log[s_heads_log_bytes++] = data[i];
    }
}

static constexpr log::backend HEADS_LOG_BACKEND = {.write = keep_heads_log};

// Whether the kept text holds `text` anywhere
static bool heads_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_heads_log_bytes; start++) {
        if (string::memcmp(s_heads_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// Writes `value` into a reply's bytes at `offset`
static void put_u32(uint8_t* bytes, uint32_t offset, uint32_t value) {
    string::memcpy(bytes + offset, &value, sizeof(value));
}

// Gives each of the three replies its part of `report`
static void put_report(heads_firmware& fw, const head_report& report) {
    put_u32(fw.replies[0], HEAD_COUNT_OFFSET, report.head_count);
    put_u32(fw.replies[1], HEAD_MASK_OFFSET, report.head_mask);
    string::memcpy(fw.replies[2] + WINDOW_MASKS_OFFSET, report.window_masks, NVIDIA_MAX_WINDOWS);
}

// Reads a report on `display`, expecting `ERR_BAD_MESSAGE` after `calls` controls with `logged` in the log and `out`
// left as it was
static void expect_refused(
    const head_report& report,
    uint32_t calls,
    const char* logged,
    const chip_display& display = TEST_DISPLAY
) {
    heads_firmware fw;
    put_report(fw, report);
    display_heads out = UNTOUCHED;

    s_heads_log_bytes = 0;
    log::set_backend(&HEADS_LOG_BACKEND);
    int32_t rc = read_display_heads(fw, TEST_DISPLAY_COMMON, display, out);
    log::set_backend(nullptr);

    EXPECT_EQ(rc, ERR_BAD_MESSAGE);
    EXPECT_EQ(fw.call_count, calls);
    EXPECT_TRUE(heads_log_contains(logged));
    EXPECT_EQ(string::memcmp(&out, &UNTOUCHED, sizeof(out)), 0);
}

// The count, the head mask and the windows' masks are asked of display common in that order, each by its own command
// with its parameters zeroed and at NVIDIA's size
TEST(nvidia_display_heads, requests_go_to_display_common_in_nvidias_order_and_layout) {
    heads_firmware earlier;
    for (uint32_t i = 0; i < 3; i++) {
        string::memset(earlier.replies[i], 0xFF, heads_firmware::MAX_BYTES);
    }

    put_report(earlier, RTX_3080_REPORT);
    heads_firmware fw;
    put_report(fw, RTX_3080_REPORT);
    display_heads earlier_heads = {};
    display_heads heads = {};

    // The earlier read fills the stack the next read's requests are expected to reuse, so a request not zeroed would
    // likely carry its replies
    ASSERT_EQ(read_display_heads(earlier, TEST_DISPLAY_COMMON, TEST_DISPLAY, earlier_heads), OK);
    ASSERT_EQ(read_display_heads(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY, heads), OK);

    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_EQ(fw.stray_count, 0u);
    const uint32_t commands[] = {NVIDIA_GET_NUM_HEADS, NVIDIA_GET_ALL_HEAD_MASK, NVIDIA_GET_WINDOW_HEADS};
    const size_t sizes[] = {NVIDIA_GET_NUM_HEADS_BYTES, NVIDIA_GET_ALL_HEAD_MASK_BYTES, NVIDIA_GET_WINDOW_HEADS_BYTES};
    for (uint32_t i = 0; i < 3; i++) {
        EXPECT_EQ(fw.calls[i].object, TEST_DISPLAY_COMMON);
        EXPECT_EQ(fw.calls[i].command, commands[i]);
        EXPECT_EQ(fw.calls[i].size, sizes[i]);
        EXPECT_TRUE(fw.calls[i].has_params);
        for (size_t byte = 0; byte < sizes[i]; byte++) {
            EXPECT_EQ(fw.calls[i].sent[byte], 0u);
        }
    }
}

// The RTX 3080's report gives each of its 4 heads two windows, head N windows 2N and 2N + 1
TEST(nvidia_display_heads, rtx_3080_report_gives_each_head_two_windows) {
    heads_firmware fw;
    put_report(fw, RTX_3080_REPORT);
    display_heads heads = UNTOUCHED;

    ASSERT_EQ(read_display_heads(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY, heads), OK);

    EXPECT_EQ(heads.count, 4u);
    EXPECT_EQ(windows_of_head(heads, 0), 0x03u);
    EXPECT_EQ(windows_of_head(heads, 1), 0x0Cu);
    EXPECT_EQ(windows_of_head(heads, 2), 0x30u);
    EXPECT_EQ(windows_of_head(heads, 3), 0xC0u);
}

// A flexible mapping gives windows 2N and 2N + 1 head N whatever their own nonzero masks, and no head to a window whose
// mask is 0, among them the windows past the chip's
TEST(nvidia_display_heads, flexible_mapping_gives_windows_2n_and_2n_plus_1_head_n) {
    heads_firmware fw;
    head_report report = {
        .head_count = 4,
        .head_mask = 0xF,
        .window_masks = {NVIDIA_FLEXIBLE_MASK, 0x02, 0x01, 0x01, 0x08, 0, 0x04, 0x03},
    };

    put_report(fw, report);
    display_heads heads = UNTOUCHED;

    ASSERT_EQ(read_display_heads(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY, heads), OK);

    EXPECT_EQ(heads.count, 4u);
    const uint8_t expected[] = {0, 0, 1, 1, 2, NO_HEAD, 3, 3};
    for (uint32_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        EXPECT_EQ(heads.window_head[window], window < 8 ? expected[window] : NO_HEAD);
    }

    EXPECT_EQ(windows_of_head(heads, 0), 0x03u);
    EXPECT_EQ(windows_of_head(heads, 2), 0x10u);
    EXPECT_EQ(windows_of_head(heads, 3), 0xC0u);
}

// Outside a flexible mapping each window serves the one head its mask names, the lowest and the highest of the chip's
TEST(nvidia_display_heads, fixed_mapping_gives_each_window_the_head_its_mask_names) {
    heads_firmware fw;
    put_report(fw, {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01, 0x02, 0x04, 0x08, 0x08, 0, 0x01}});
    display_heads heads = UNTOUCHED;

    ASSERT_EQ(read_display_heads(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY, heads), OK);

    EXPECT_EQ(heads.count, 4u);
    const uint8_t expected[] = {0, 1, 2, 3, 3, NO_HEAD, 0, NO_HEAD};
    for (uint32_t window = 0; window < 8; window++) {
        EXPECT_EQ(heads.window_head[window], expected[window]);
    }

    EXPECT_EQ(windows_of_head(heads, 0), 0x41u);
    EXPECT_EQ(windows_of_head(heads, 3), 0x18u);
}

// The heads a report leaves the display engine end at the first head the mask lacks, with all served, or in either
// mapping at the first no window serves, with all in the mask, and the windows of the heads past it serve none
TEST(nvidia_display_heads, heads_past_a_gap_are_dropped_with_their_windows) {
    heads_firmware mask_gap;
    head_report mask_gap_report = RTX_3080_REPORT;
    mask_gap_report.head_mask = 0xB;
    put_report(mask_gap, mask_gap_report);

    heads_firmware window_gap;
    put_report(window_gap, {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01, 0x02, 0x08, 0x02}});

    heads_firmware flexible_window_gap;
    head_report flexible_window_gap_report = RTX_3080_REPORT;
    flexible_window_gap_report.window_masks[4] = 0;
    flexible_window_gap_report.window_masks[5] = 0;
    put_report(flexible_window_gap, flexible_window_gap_report);

    display_heads from_mask = UNTOUCHED;
    display_heads from_windows = UNTOUCHED;
    display_heads from_flexible_windows = UNTOUCHED;
    ASSERT_EQ(read_display_heads(mask_gap, TEST_DISPLAY_COMMON, TEST_DISPLAY, from_mask), OK);
    ASSERT_EQ(read_display_heads(window_gap, TEST_DISPLAY_COMMON, TEST_DISPLAY, from_windows), OK);
    ASSERT_EQ(read_display_heads(flexible_window_gap, TEST_DISPLAY_COMMON, TEST_DISPLAY, from_flexible_windows), OK);

    EXPECT_EQ(from_mask.count, 2u);
    EXPECT_EQ(from_mask.window_head[3], 1u);
    EXPECT_EQ(from_mask.window_head[6], NO_HEAD);
    EXPECT_EQ(windows_of_head(from_mask, 3), 0u);

    EXPECT_EQ(from_windows.count, 2u);
    EXPECT_EQ(from_windows.window_head[2], NO_HEAD);
    EXPECT_EQ(windows_of_head(from_windows, 1), 0x0Au);

    EXPECT_EQ(from_flexible_windows.count, 2u);
    EXPECT_EQ(from_flexible_windows.window_head[6], NO_HEAD);
    EXPECT_EQ(from_flexible_windows.window_head[7], NO_HEAD);
    EXPECT_EQ(windows_of_head(from_flexible_windows, 1), 0x0Cu);
    EXPECT_EQ(windows_of_head(from_flexible_windows, 3), 0u);
}

// A count below the packed heads (those from head 0 up to the first gap) keeps only that many
TEST(nvidia_display_heads, count_below_the_packed_heads_limits_them) {
    heads_firmware fw;
    head_report report = RTX_3080_REPORT;
    report.head_count = 2;
    put_report(fw, report);
    display_heads heads = UNTOUCHED;

    ASSERT_EQ(read_display_heads(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY, heads), OK);

    EXPECT_EQ(heads.count, 2u);
    EXPECT_EQ(heads.window_head[3], 1u);
    EXPECT_EQ(heads.window_head[4], NO_HEAD);
    EXPECT_EQ(windows_of_head(heads, 2), 0u);
}

// A count of 0, the count just past the chip's heads and one whose low byte alone would pass are refused before the
// head mask is asked for
TEST(nvidia_display_heads, count_of_none_or_past_the_chip_is_refused) {
    head_report none = RTX_3080_REPORT;
    none.head_count = 0;
    head_report one_past = RTX_3080_REPORT;
    one_past.head_count = 5;
    head_report past_its_low_byte = RTX_3080_REPORT;
    past_its_low_byte.head_count = 0x104;

    expect_refused(none, 1, "nvidia: display common reports no heads the display engine can use");
    expect_refused(one_past, 1, "reports 5 heads the display engine can use, more than the chip's 4 heads");
    expect_refused(past_its_low_byte, 1, "reports 260 heads the display engine can use, more than the chip's 4");
}

// A head mask naming the head just past the chip's or head 31, the highest it can name, is refused before the windows
// are asked for
TEST(nvidia_display_heads, head_mask_past_the_chip_is_refused) {
    head_report one_past = RTX_3080_REPORT;
    one_past.head_mask = 0x1F;
    head_report highest = RTX_3080_REPORT;
    highest.head_mask = 0x8000000F;

    expect_refused(one_past, 2, "reports heads 0x1F usable, some past the chip's 4 heads");
    expect_refused(highest, 2, "reports heads 0x8000000F usable, some past the chip's 4 heads");
}

// A nonzero mask for the window just past the chip's or the last window is refused, flexible or not
TEST(nvidia_display_heads, mask_for_a_window_past_the_chip_is_refused) {
    head_report flexible_past = RTX_3080_REPORT;
    flexible_past.window_masks[8] = NVIDIA_FLEXIBLE_MASK;
    head_report flexible_last = RTX_3080_REPORT;
    flexible_last.window_masks[31] = 0x01;
    head_report fixed_past = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01}};
    fixed_past.window_masks[8] = 0x02;

    expect_refused(flexible_past, 3, "reports heads 0xFF for window 8, past the chip's 8 windows");
    expect_refused(flexible_last, 3, "reports heads 0x1 for window 31, past the chip's 8 windows");
    expect_refused(fixed_past, 3, "reports heads 0x2 for window 8, past the chip's 8 windows");
}

// Outside a flexible mapping a window's mask of two heads is refused, as is window 0's mask one bit short of the
// flexible one at either end and a later window's flexible mask, as only window 0's mask makes a mapping flexible
TEST(nvidia_display_heads, fixed_mapping_refuses_several_heads_for_a_window) {
    head_report two_heads = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x03}};
    head_report two_heads_later = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01, 0x03}};
    head_report short_of_flexible_low = {.head_count = 4, .head_mask = 0xF, .window_masks = {0xFE}};
    head_report short_of_flexible_high = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x7F}};
    head_report flexible_later = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01, 0xFF}};
    head_report flexible_after_none = {.head_count = 4, .head_mask = 0xF, .window_masks = {0, 0xFF}};

    expect_refused(two_heads, 3, "reports heads 0x3 for window 0, more than one while window 0's mask is not the 0xFF");
    expect_refused(two_heads_later, 3, "reports heads 0x3 for window 1, more than one");
    expect_refused(short_of_flexible_low, 3, "reports heads 0xFE for window 0, more than one");
    expect_refused(short_of_flexible_high, 3, "reports heads 0x7F for window 0, more than one");
    expect_refused(flexible_later, 3, "reports heads 0xFF for window 1, more than one");
    expect_refused(flexible_after_none, 3, "reports heads 0xFF for window 1, more than one");
}

// Outside a flexible mapping a window's mask naming the head just past the chip's or head 7, the highest a mask can
// name, is refused
TEST(nvidia_display_heads, fixed_mapping_refuses_a_head_past_the_chip) {
    head_report one_past = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x10}};
    head_report highest = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01, 0x80}};

    expect_refused(one_past, 3, "reports head 4 for window 0, past the chip's 4 heads");
    expect_refused(highest, 3, "reports head 7 for window 1, past the chip's 4 heads");
}

// A report that leaves the display engine no head is refused, each cause on its own line: a mask without head 0, served
// or not, no window serving head 0, or no window serving any head
TEST(nvidia_display_heads, report_leaving_the_engine_no_head_is_refused) {
    head_report no_head_0 = RTX_3080_REPORT;
    no_head_0.head_mask = 0xE;
    head_report no_head_0_none_served = {.head_count = 4, .head_mask = 0xE, .window_masks = {}};
    head_report head_0_unserved = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x02, 0x04}};
    head_report none_served = {.head_count = 4, .head_mask = 0xF, .window_masks = {}};

    expect_refused(no_head_0, 3, "reports heads 0xE usable, without head 0, which leaves the display engine no head");
    expect_refused(no_head_0_none_served, 3, "reports heads 0xE usable, without head 0");
    expect_refused(head_0_unserved, 3, "reports windows serving heads 0x6, without head 0");
    expect_refused(none_served, 3, "reports windows serving heads 0x0, without head 0");
}

// A report failing several checks is refused by the first: a window's bound before several heads, a head past the chip
// or the packing, and several heads before a head past the chip
TEST(nvidia_display_heads, the_first_check_a_report_fails_refuses_it) {
    head_report past_with_two_heads = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01}};
    past_with_two_heads.window_masks[8] = 0x03;
    head_report past_with_head_past = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x01}};
    past_with_head_past.window_masks[8] = 0x10;

    head_report two_heads_past = {.head_count = 4, .head_mask = 0xF, .window_masks = {0x30}};
    head_report past_and_unpackable = RTX_3080_REPORT;
    past_and_unpackable.head_mask = 0xE;
    past_and_unpackable.window_masks[8] = 0x01;

    expect_refused(past_with_two_heads, 3, "reports heads 0x3 for window 8, past the chip's 8 windows");
    expect_refused(past_with_head_past, 3, "reports heads 0x10 for window 8, past the chip's 8 windows");
    expect_refused(two_heads_past, 3, "reports heads 0x30 for window 0, more than one");
    expect_refused(past_and_unpackable, 3, "reports heads 0x1 for window 8, past the chip's 8 windows");
}

// A chip of 2 heads, fewer than `DISPLAY_HEADS_MAX`, with the classes left 0
static constexpr chip_display TWO_HEAD_DISPLAY = {
    .head_count         = 2,
    .window_count       = 8,
    .display_class      = 0,
    .core_channel_class = 0,
};

// The chip's own head count, not `DISPLAY_HEADS_MAX`, bounds the count, the head mask and a fixed window's head
TEST(nvidia_display_heads, a_chip_of_fewer_heads_bounds_the_report) {
    head_report count_past = {.head_count = 3, .head_mask = 0x3, .window_masks = {0x01, 0x02}};
    head_report mask_past = {.head_count = 2, .head_mask = 0x7, .window_masks = {0x01, 0x02}};
    head_report window_head_past = {.head_count = 2, .head_mask = 0x3, .window_masks = {0x01, 0x04}};

    expect_refused(count_past, 1, "3 heads the display engine can use, more than the chip's 2", TWO_HEAD_DISPLAY);
    expect_refused(mask_past, 2, "reports heads 0x7 usable, some past the chip's 2 heads", TWO_HEAD_DISPLAY);
    expect_refused(window_head_past, 3, "reports head 2 for window 1, past the chip's 2 heads", TWO_HEAD_DISPLAY);
}

// A chip of 2 heads takes the RTX 3080's report cut to 2 heads, keeping the windows of heads 0 and 1 and leaving
// windows 4 to 7 no head
TEST(nvidia_display_heads, a_chip_of_fewer_heads_takes_a_flexible_report_cut_to_them) {
    heads_firmware fw;
    head_report report = RTX_3080_REPORT;
    report.head_count = 2;
    report.head_mask = 0x3;
    put_report(fw, report);
    display_heads heads = UNTOUCHED;

    ASSERT_EQ(read_display_heads(fw, TEST_DISPLAY_COMMON, TWO_HEAD_DISPLAY, heads), OK);

    EXPECT_EQ(heads.count, 2u);
    EXPECT_EQ(windows_of_head(heads, 0), 0x03u);
    EXPECT_EQ(windows_of_head(heads, 1), 0x0Cu);
    for (uint32_t window = 4; window < 8; window++) {
        EXPECT_EQ(heads.window_head[window], NO_HEAD);
    }
}

// Each request's own error is returned at once with `out` left as it was, the window control's being `ERR_FIRMWARE`,
// the error that carries a refusal such as its NOT_SUPPORTED
TEST(nvidia_display_heads, failed_requests_are_passed_up) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_FIRMWARE};
    for (uint32_t failing = 0; failing < 3; failing++) {
        heads_firmware fw;
        put_report(fw, RTX_3080_REPORT);
        fw.results[failing] = errors[failing];
        display_heads out = UNTOUCHED;

        EXPECT_EQ(read_display_heads(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY, out), errors[failing]);

        EXPECT_EQ(fw.call_count, failing + 1);
        EXPECT_EQ(string::memcmp(&out, &UNTOUCHED, sizeof(out)), 0);
    }
}

// `windows_of_head` gives no window for a head at or past the count or for `NO_HEAD`, even when windows record it
TEST(nvidia_display_heads, windows_of_head_past_the_count_are_none) {
    display_heads heads = {};
    string::memset(heads.window_head, NO_HEAD, sizeof(heads.window_head));
    heads.count = 1;
    heads.window_head[0] = 0;
    heads.window_head[1] = 1;

    EXPECT_EQ(windows_of_head(heads, 0), 0x1u);
    EXPECT_EQ(windows_of_head(heads, 1), 0u);
    EXPECT_EQ(windows_of_head(heads, NO_HEAD), 0u);
}

// The chip table's bound on heads is NVIDIA's
TEST(nvidia_display_heads, display_heads_max_is_nvidias) {
    EXPECT_EQ(DISPLAY_HEADS_MAX, NVIDIA_MAX_HEADS);
}
