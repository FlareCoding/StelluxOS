// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_info.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_info);

using drivers::nvidia::chip_display;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::read_present_windows;
namespace rm = drivers::nvidia::rm;

// NVIDIA's command for the display's static information, the information's size and the word holding the present
// windows, restated here so the tests hold the request to NVIDIA's layout rather than to the driver's
static constexpr uint32_t NVIDIA_GET_DISPLAY_STATIC_INFO = 0x20800A01;
static constexpr size_t   DISPLAY_STATIC_INFO_BYTES      = 28;
static constexpr uint32_t DISPLAY_STATIC_INFO_WORDS      = DISPLAY_STATIC_INFO_BYTES / 4;
static constexpr uint32_t PRESENT_WINDOWS_WORD           = 1;

// A mask naming only some of the windows, a value each call's output holds before the call, so a test sees whether the
// call wrote it, and GA102's heads and windows, with the classes left 0, as reading the windows never uses them
static constexpr uint32_t     SPARSE_WINDOWS    = 0xA5;
static constexpr uint32_t     UNTOUCHED_WINDOWS = 0xDEADBEEF;
static constexpr chip_display GA102_DISPLAY     = {
    .head_count         = 4,
    .window_count       = 8,
    .display_class      = 0,
    .core_channel_class = 0,
    .displayport        = {},
};

// A firmware that answers internal controls with `result` and, on `OK`, the words of `reply`, recording what each
// carried, and counts calls of `allocate`, `control`, `release` and `new_handle`, which no function under test makes
class static_info_firmware : public firmware {
public:
    uint32_t reply[DISPLAY_STATIC_INFO_WORDS] = {};
    int32_t  result = OK;
    uint32_t command = 0;
    size_t   size = 0;
    bool     has_params = false;
    uint32_t sent[DISPLAY_STATIC_INFO_WORDS] = {};
    uint32_t control_count = 0;
    uint32_t other_count = 0;

    // Every word but the present windows reads all ones, so a mask read from another word is refused on a chip of fewer
    // than 32 windows
    explicit static_info_firmware(uint32_t present_windows) {
        for (uint32_t i = 0; i < DISPLAY_STATIC_INFO_WORDS; i++) {
            reply[i] = 0xFFFFFFFF;
        }

        reply[PRESENT_WINDOWS_WORD] = present_windows;
    }

    int32_t internal_control(uint32_t command_sent, void* params, size_t params_size) override {
        control_count++;
        command = command_sent;
        size = params_size;
        has_params = params != nullptr;
        size_t copied = params_size < sizeof(sent) ? params_size : sizeof(sent);
        if (params != nullptr) {
            string::memcpy(sent, params, copied);
        }

        if (result == OK && params != nullptr) {
            string::memcpy(params, reply, copied);
        }

        return result;
    }

    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override {
        other_count++;
        return OK;
    }

    int32_t control(rm::handle, uint32_t, void*, size_t) override {
        other_count++;
        return OK;
    }

    int32_t release(rm::handle) override {
        other_count++;
        return OK;
    }

    rm::handle new_handle() override {
        other_count++;
        return 0;
    }

    rm::handle client() const override {
        return 0;
    }
};

// One request goes out, NVIDIA's command with the information's 28 bytes zeroed, and the windows come from NVIDIA's
// field of the reply
TEST(nvidia_display_info, windows_come_from_nvidias_field_of_one_zeroed_request) {
    static_info_firmware earlier(SPARSE_WINDOWS);
    static_info_firmware fw(SPARSE_WINDOWS);
    uint32_t earlier_windows = UNTOUCHED_WINDOWS;
    uint32_t present_windows = UNTOUCHED_WINDOWS;

    // The earlier read leaves its reply in the frame the next read's request takes, so a request not zeroed carries it
    int32_t earlier_rc = read_present_windows(earlier, GA102_DISPLAY, earlier_windows);
    int32_t rc = read_present_windows(fw, GA102_DISPLAY, present_windows);

    EXPECT_EQ(earlier_rc, OK);
    EXPECT_EQ(rc, OK);
    EXPECT_EQ(present_windows, SPARSE_WINDOWS);
    EXPECT_EQ(fw.control_count, 1u);
    EXPECT_EQ(fw.other_count, 0u);
    EXPECT_EQ(fw.command, NVIDIA_GET_DISPLAY_STATIC_INFO);
    EXPECT_EQ(fw.size, DISPLAY_STATIC_INFO_BYTES);
    EXPECT_TRUE(fw.has_params);
    for (uint32_t i = 0; i < DISPLAY_STATIC_INFO_WORDS; i++) {
        EXPECT_EQ(fw.sent[i], 0u);
    }
}

// A chip one window short of NVIDIA's most windows, with GA102's heads and the classes left 0
static constexpr chip_display ONE_SHORT_DISPLAY = {
    .head_count         = 4,
    .window_count       = 31,
    .display_class      = 0,
    .core_channel_class = 0,
    .displayport        = {},
};

// A chip of NVIDIA's most windows, with GA102's heads and the classes left 0
static constexpr chip_display MOST_WINDOWS_DISPLAY = {
    .head_count         = 4,
    .window_count       = 32,
    .display_class      = 0,
    .core_channel_class = 0,
    .displayport        = {},
};

// Every window of the chip or none is accepted, up to every bit on a chip of NVIDIA's most windows
TEST(nvidia_display_info, every_window_of_the_chip_or_none_is_accepted) {
    static_info_firmware every(0xFF);
    static_info_firmware none(0);
    static_info_firmware one_short(0x7FFFFFFF);
    static_info_firmware widest(0xFFFFFFFF);
    uint32_t every_windows = UNTOUCHED_WINDOWS;
    uint32_t no_windows = UNTOUCHED_WINDOWS;
    uint32_t one_short_windows = UNTOUCHED_WINDOWS;
    uint32_t widest_windows = UNTOUCHED_WINDOWS;

    EXPECT_EQ(read_present_windows(every, GA102_DISPLAY, every_windows), OK);
    EXPECT_EQ(read_present_windows(none, GA102_DISPLAY, no_windows), OK);
    EXPECT_EQ(read_present_windows(one_short, ONE_SHORT_DISPLAY, one_short_windows), OK);
    EXPECT_EQ(read_present_windows(widest, MOST_WINDOWS_DISPLAY, widest_windows), OK);

    EXPECT_EQ(every_windows, 0xFFu);
    EXPECT_EQ(no_windows, 0u);
    EXPECT_EQ(one_short_windows, 0x7FFFFFFFu);
    EXPECT_EQ(widest_windows, 0xFFFFFFFFu);
}

// The text logged while `TEXT_KEEPING_BACKEND` is set
static char     s_logged_text[512] = {};
static uint32_t s_logged_bytes     = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_logged_text(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_logged_bytes < sizeof(s_logged_text); i++) {
        s_logged_text[s_logged_bytes++] = data[i];
    }
}

static constexpr log::backend TEXT_KEEPING_BACKEND = {.write = keep_logged_text};

// Whether the kept text holds `text` anywhere
static bool logged_text_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_logged_bytes; start++) {
        if (string::memcmp(s_logged_text + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// The window just past GA102's, bit 31 on GA102 and bit 31 on a chip one window short of NVIDIA's most are each refused
// with the mask and the chip's count logged, leaving each output unwritten
TEST(nvidia_display_info, a_window_past_the_chips_is_refused_and_logged) {
    static_info_firmware next(0x1A5);
    static_info_firmware top(0x80000000);
    static_info_firmware top_of_one_short(0x80000000);
    uint32_t next_windows = UNTOUCHED_WINDOWS;
    uint32_t top_windows = UNTOUCHED_WINDOWS;
    uint32_t top_of_one_short_windows = UNTOUCHED_WINDOWS;

    s_logged_bytes = 0;
    log::set_backend(&TEXT_KEEPING_BACKEND);
    int32_t next_rc = read_present_windows(next, GA102_DISPLAY, next_windows);
    int32_t top_rc = read_present_windows(top, GA102_DISPLAY, top_windows);
    int32_t top_of_one_short_rc = read_present_windows(top_of_one_short, ONE_SHORT_DISPLAY, top_of_one_short_windows);
    log::set_backend(nullptr);

    EXPECT_EQ(next_rc, ERR_BAD_MESSAGE);
    EXPECT_EQ(top_rc, ERR_BAD_MESSAGE);
    EXPECT_EQ(top_of_one_short_rc, ERR_BAD_MESSAGE);
    EXPECT_TRUE(logged_text_contains("reports windows 0x1A5 present, some past the chip's 8 windows"));
    EXPECT_TRUE(logged_text_contains("reports windows 0x80000000 present, some past the chip's 8 windows"));
    EXPECT_TRUE(logged_text_contains("reports windows 0x80000000 present, some past the chip's 31 windows"));
    EXPECT_EQ(next_windows, UNTOUCHED_WINDOWS);
    EXPECT_EQ(top_windows, UNTOUCHED_WINDOWS);
    EXPECT_EQ(top_of_one_short_windows, UNTOUCHED_WINDOWS);
}

// A refused or unanswered control passes its error up, leaving each output unwritten
TEST(nvidia_display_info, a_failed_control_passes_its_error_up) {
    static_info_firmware refused(SPARSE_WINDOWS);
    static_info_firmware unanswered(SPARSE_WINDOWS);
    refused.result = ERR_FIRMWARE;
    unanswered.result = ERR_TIMEOUT;
    uint32_t refused_windows = UNTOUCHED_WINDOWS;
    uint32_t unanswered_windows = UNTOUCHED_WINDOWS;

    EXPECT_EQ(read_present_windows(refused, GA102_DISPLAY, refused_windows), ERR_FIRMWARE);
    EXPECT_EQ(read_present_windows(unanswered, GA102_DISPLAY, unanswered_windows), ERR_TIMEOUT);

    EXPECT_EQ(refused_windows, UNTOUCHED_WINDOWS);
    EXPECT_EQ(unanswered_windows, UNTOUCHED_WINDOWS);
    EXPECT_EQ(refused.control_count, 1u);
    EXPECT_EQ(unanswered.control_count, 1u);
}
