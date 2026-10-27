// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/display_edid.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_edid);

using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::read_output_edid;
namespace rm = drivers::nvidia::rm;

// NVIDIA's GET_EDID_V2, its parameters' size, the offsets of the fields read from or sent in them and the most EDID
// bytes a reply carries, restated so the tests hold the requests to NVIDIA's layout
static constexpr uint32_t NVIDIA_GET_EDID_V2       = 0x730245;
static constexpr size_t   NVIDIA_GET_EDID_V2_BYTES = 2064;
static constexpr uint32_t DISPLAY_ID_OFFSET        = 4;
static constexpr uint32_t EDID_BYTES_OFFSET        = 8;
static constexpr uint32_t EDID_OFFSET              = 16;
static constexpr uint32_t EDID_BUFFER_BYTES        = 2048;

// A handle for display common unlike those `gsp` gives, and an output's display id
static constexpr rm::handle TEST_DISPLAY_COMMON = 0x0005000B;
static constexpr uint32_t   TEST_DISPLAY_ID     = 0x100;

// The AOC AG241QG4's base block with its serial number (bytes 12 to 15) and extension count (byte 126) zeroed and its
// checksum recomputed, giving maker "AOC", name "AG241QG4" and two timings, the first preferred
static constexpr uint8_t AOC_BASE_BLOCK[edid::BLOCK_BYTES] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x05, 0xE3, 0x10, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x26, 0x1C, 0x01, 0x04, 0xA5, 0x35, 0x28, 0x78, 0x3B, 0xE1, 0x61, 0xA4, 0x57, 0x53, 0xA0, 0x27,
    0x0D, 0x50, 0x54, 0xBF, 0xEF, 0x00, 0xD1, 0xC0, 0x81, 0x80, 0x31, 0x68, 0x31, 0x7C, 0x45, 0x68,
    0x45, 0x7C, 0x61, 0x68, 0x61, 0x7C, 0x56, 0x5E, 0x00, 0xA0, 0xA0, 0xA0, 0x29, 0x50, 0x30, 0x20,
    0x35, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E, 0x40, 0xE7, 0x00, 0x6A, 0xA0, 0xA0, 0x67, 0x50,
    0x08, 0x20, 0x98, 0x04, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x1E,
    0x92, 0xE6, 0xE6, 0x3C, 0x01, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC,
    0x00, 0x41, 0x47, 0x32, 0x34, 0x31, 0x51, 0x47, 0x34, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x00, 0xCD,
};

// The two timings of the AOC's base block, 2560 by 1440 at about 60 Hz and at about 144 Hz
static constexpr edid::timing AOC_60_HZ  = {241500, 2560, 48, 32, 80, 1440, 3, 5, 33, true, true};
static constexpr edid::timing AOC_144_HZ = {592000, 2560, 8, 32, 66, 1440, 25, 8, 70, true, false};

// A firmware that answers GET_EDID_V2 with the first `reply_copied_bytes` bytes of `reply` and `reply_edid_bytes` as
// the EDID's length, writing a reply only when `result` is `OK`, and records the first `MAX_CALLS` controls sent to it
class edid_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 4;

    struct call {
        rm::handle object;
        uint32_t   command;
        size_t     size;
        uint32_t   display_id;
        bool       is_zero_but_display_id; // every byte of the parameters but the display id's is 0
    };

    call     calls[MAX_CALLS] = {};
    uint32_t call_count = 0;
    uint32_t stray_count = 0;

    const uint8_t* reply = AOC_BASE_BLOCK;
    uint32_t       reply_copied_bytes = edid::BLOCK_BYTES;
    uint32_t       reply_edid_bytes = edid::BLOCK_BYTES;
    int32_t        result = OK;

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        record(object, command, params, size);
        if (command != NVIDIA_GET_EDID_V2 || params == nullptr || size != NVIDIA_GET_EDID_V2_BYTES) {
            stray_count++;
            return OK;
        }

        if (result != OK) {
            return result;
        }

        uint8_t* bytes = static_cast<uint8_t*>(params);
        string::memcpy(bytes + EDID_BYTES_OFFSET, &reply_edid_bytes, sizeof(uint32_t));
        string::memcpy(bytes + EDID_OFFSET, reply, reply_copied_bytes);

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
        if (params == nullptr || size < DISPLAY_ID_OFFSET + sizeof(uint32_t)) {
            return;
        }

        const uint8_t* bytes = static_cast<const uint8_t*>(params);
        string::memcpy(&recorded.display_id, bytes + DISPLAY_ID_OFFSET, sizeof(uint32_t));
        recorded.is_zero_but_display_id = true;
        for (size_t i = 0; i < size; i++) {
            bool is_display_id = i >= DISPLAY_ID_OFFSET && i < DISPLAY_ID_OFFSET + sizeof(uint32_t);
            if (!is_display_id && bytes[i] != 0) {
                recorded.is_zero_but_display_id = false;
            }
        }
    }
};

// The monitor each test reads into, kept off the stack as `edid::monitor` asks
static edid::monitor s_monitor = {};

// The byte every byte of `s_monitor` holds before a read, so a test sees whether the read wrote it
static constexpr uint8_t UNTOUCHED = 0xEE;

// Sets every byte of `s_monitor` to `value`
static void fill_monitor(uint8_t value) {
    string::memset(&s_monitor, value, sizeof(s_monitor));
}

// Whether every byte of `s_monitor` holds `value`
static bool monitor_holds_only(uint8_t value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&s_monitor);
    for (size_t i = 0; i < sizeof(s_monitor); i++) {
        if (bytes[i] != value) {
            return false;
        }
    }

    return true;
}

// The text logged while `EDID_LOG_BACKEND` is set
static char     s_edid_log[2048] = {};
static uint32_t s_edid_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_edid_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_edid_log_bytes < sizeof(s_edid_log); i++) {
        s_edid_log[s_edid_log_bytes++] = data[i];
    }
}

static constexpr log::backend EDID_LOG_BACKEND = {.write = keep_edid_log};

// Whether the kept text holds `text` anywhere
static bool edid_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_edid_log_bytes; start++) {
        if (string::memcmp(s_edid_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// Reads the test display id's EDID into `s_monitor` with the log kept
static int32_t read_logged(edid_firmware& fw, bool& is_parsed) {
    s_edid_log_bytes = 0;
    log::set_backend(&EDID_LOG_BACKEND);
    int32_t rc = read_output_edid(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, s_monitor, is_parsed);
    log::set_backend(nullptr);

    return rc;
}

// The request goes to display common in NVIDIA's command and size, zeroed but for the display id, so its flags ask the
// firmware to read the EDID from the monitor
TEST(nvidia_display_edid, request_goes_to_display_common_in_nvidias_layout) {
    edid_firmware fw;
    bool is_parsed = false;

    ASSERT_EQ(read_output_edid(fw, TEST_DISPLAY_COMMON, 0x800, s_monitor, is_parsed), OK);

    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_EQ(fw.stray_count, 0u);

    EXPECT_EQ(fw.calls[0].object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(fw.calls[0].command, NVIDIA_GET_EDID_V2);
    EXPECT_EQ(fw.calls[0].size, NVIDIA_GET_EDID_V2_BYTES);
    EXPECT_EQ(fw.calls[0].display_id, 0x800u);
    EXPECT_TRUE(fw.calls[0].is_zero_but_display_id);
}

// An EDID that parses gives the monitor's maker, name and timings, with `is_parsed` set and nothing logged
TEST(nvidia_display_edid, an_edid_that_parses_gives_the_monitor) {
    edid_firmware fw;
    bool is_parsed = false;
    fill_monitor(UNTOUCHED);

    ASSERT_EQ(read_logged(fw, is_parsed), OK);

    EXPECT_TRUE(is_parsed);
    EXPECT_EQ(s_edid_log_bytes, 0u);

    EXPECT_STREQ(s_monitor.maker, "AOC");
    EXPECT_STREQ(s_monitor.name, "AG241QG4");
    ASSERT_EQ(s_monitor.timing_count, 2u);
    EXPECT_TRUE(s_monitor.timings[0] == AOC_60_HZ);
    EXPECT_TRUE(s_monitor.timings[1] == AOC_144_HZ);
    EXPECT_TRUE(s_monitor.first_timing_is_preferred);
}

// A reply filling the buffer, the AOC's base block followed by zeros
static uint8_t s_filled_buffer[EDID_BUFFER_BYTES] = {};

// The parse takes as many bytes as the reply reports, up to the 2048 its buffer holds, so an EDID filling the buffer
// parses and one reported a byte shorter than its base block does not
TEST(nvidia_display_edid, the_parse_takes_the_bytes_the_reply_reports) {
    string::memcpy(s_filled_buffer, AOC_BASE_BLOCK, sizeof(AOC_BASE_BLOCK));

    edid_firmware filled;
    filled.reply = s_filled_buffer;
    filled.reply_copied_bytes = EDID_BUFFER_BYTES;
    filled.reply_edid_bytes = EDID_BUFFER_BYTES;
    bool is_parsed = false;

    ASSERT_EQ(read_output_edid(filled, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, s_monitor, is_parsed), OK);
    EXPECT_TRUE(is_parsed);
    EXPECT_EQ(s_monitor.timing_count, 2u);

    edid_firmware short_by_one;
    short_by_one.reply_edid_bytes = edid::BLOCK_BYTES - 1;

    ASSERT_EQ(read_logged(short_by_one, is_parsed), OK);
    EXPECT_FALSE(is_parsed);
    EXPECT_TRUE(edid_log_contains("nvidia: display id 0x100's EDID of 127 bytes is shorter than one block"));
}

// A refused request is logged and leaves `is_parsed` cleared, with `out` as it was
TEST(nvidia_display_edid, a_refused_request_is_logged) {
    edid_firmware fw;
    fw.result = ERR_FIRMWARE;
    bool is_parsed = true;
    fill_monitor(UNTOUCHED);

    ASSERT_EQ(read_logged(fw, is_parsed), OK);

    EXPECT_FALSE(is_parsed);
    EXPECT_EQ(fw.call_count, 1u);
    EXPECT_TRUE(monitor_holds_only(UNTOUCHED));
    EXPECT_TRUE(edid_log_contains("nvidia: display common refused display id 0x100's EDID"));
}

// Any other error of the request ends the read with that error, `out` left as it was
TEST(nvidia_display_edid, other_errors_of_the_request_end_the_read) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_BAD_MESSAGE, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        edid_firmware fw;
        fw.result = error;
        bool is_parsed = true;
        fill_monitor(UNTOUCHED);

        EXPECT_EQ(read_output_edid(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, s_monitor, is_parsed), error);

        EXPECT_FALSE(is_parsed);
        EXPECT_EQ(fw.call_count, 1u);
        EXPECT_TRUE(monitor_holds_only(UNTOUCHED));
    }
}

// A reply reporting more EDID bytes than its buffer holds is logged and leaves `is_parsed` cleared, with `out` as it
// was, including a length whose low 16 bits alone would parse
TEST(nvidia_display_edid, a_reply_longer_than_its_buffer_is_logged) {
    struct long_reply {
        uint32_t    length;
        const char* logged;
    };

    const long_reply cases[] = {
        {EDID_BUFFER_BYTES + 1, "EDID as 2049 bytes, more than the 2048 its reply holds"},
        {0x10000 + edid::BLOCK_BYTES, "EDID as 65664 bytes, more than the 2048 its reply holds"},
        {0xFFFFFFFF, "EDID as 4294967295 bytes, more than the 2048 its reply holds"},
    };

    for (const long_reply& reply : cases) {
        edid_firmware fw;
        fw.reply_edid_bytes = reply.length;
        bool is_parsed = true;
        fill_monitor(UNTOUCHED);

        ASSERT_EQ(read_logged(fw, is_parsed), OK);

        EXPECT_FALSE(is_parsed);
        EXPECT_TRUE(monitor_holds_only(UNTOUCHED));
        EXPECT_TRUE(edid_log_contains("nvidia: display common reports display id 0x100's EDID as "));
        EXPECT_TRUE(edid_log_contains(reply.logged));
    }
}

// An EDID failing any of the parser's checks is logged with that check's cause and leaves `is_parsed` cleared, with
// every field of `out` zero as the parse leaves it
TEST(nvidia_display_edid, an_edid_failing_to_parse_is_logged_with_its_cause) {
    uint8_t bad_header[edid::BLOCK_BYTES];
    string::memcpy(bad_header, AOC_BASE_BLOCK, sizeof(AOC_BASE_BLOCK));
    bad_header[0] = 0x01;
    bad_header[127] = static_cast<uint8_t>(bad_header[127] - 1);

    uint8_t bad_checksum[edid::BLOCK_BYTES];
    string::memcpy(bad_checksum, AOC_BASE_BLOCK, sizeof(AOC_BASE_BLOCK));
    bad_checksum[127] = static_cast<uint8_t>(bad_checksum[127] + 1);

    uint8_t bad_version[edid::BLOCK_BYTES];
    string::memcpy(bad_version, AOC_BASE_BLOCK, sizeof(AOC_BASE_BLOCK));
    bad_version[18] = 2;
    bad_version[127] = static_cast<uint8_t>(bad_version[127] - 1);

    struct bad_edid {
        const uint8_t* bytes;
        uint32_t       length;
        const char*    logged;
    };

    const bad_edid cases[] = {
        {AOC_BASE_BLOCK, 0, "display id 0x100's EDID of 0 bytes is shorter than one block"},
        {bad_header, edid::BLOCK_BYTES, "display id 0x100's EDID of 128 bytes does not start with the EDID header"},
        {bad_checksum, edid::BLOCK_BYTES, "display id 0x100's EDID of 128 bytes fails its checksum"},
        {bad_version, edid::BLOCK_BYTES, "display id 0x100's EDID of 128 bytes is not of version 1"},
    };

    for (const bad_edid& bad : cases) {
        edid_firmware fw;
        fw.reply = bad.bytes;
        fw.reply_edid_bytes = bad.length;
        bool is_parsed = true;
        fill_monitor(UNTOUCHED);

        ASSERT_EQ(read_logged(fw, is_parsed), OK);

        EXPECT_FALSE(is_parsed);
        EXPECT_EQ(fw.call_count, 1u);
        EXPECT_TRUE(monitor_holds_only(0));
        EXPECT_TRUE(edid_log_contains(bad.logged));
    }
}
