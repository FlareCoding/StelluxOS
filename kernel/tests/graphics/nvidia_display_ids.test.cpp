// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_ids.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_ids);

using drivers::nvidia::display_ids;
using drivers::nvidia::DISPLAY_IDS_MAX;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::read_display_ids;
namespace rm = drivers::nvidia::rm;

// NVIDIA's display id controls, their parameters' sizes and the offsets of the fields their requests send or their
// replies give, restated so the tests hold the requests to NVIDIA's layouts
static constexpr uint32_t NVIDIA_GET_SUPPORTED           = 0x730120;
static constexpr size_t   NVIDIA_GET_SUPPORTED_BYTES     = 12;
static constexpr uint32_t SUPPORTED_MASK_OFFSET          = 4;
static constexpr uint32_t NVIDIA_GET_CONNECT_STATE       = 0x730122;
static constexpr size_t   NVIDIA_GET_CONNECT_STATE_BYTES = 16;
static constexpr uint32_t CONNECT_MASK_OFFSET            = 8;
static constexpr uint32_t RETRY_TIME_OFFSET              = 12;
static constexpr uint32_t NVIDIA_GET_TYPE                = 0x730240;
static constexpr size_t   NVIDIA_GET_TYPE_BYTES          = 12;
static constexpr uint32_t DISPLAY_ID_OFFSET              = 4;
static constexpr uint32_t KIND_OFFSET                    = 8;

// NVIDIA's display kinds, from ctrl0073specific.h
static constexpr uint32_t KIND_UNKNOWN = 0;
static constexpr uint32_t KIND_CRT     = 1;
static constexpr uint32_t KIND_DFP     = 2;
static constexpr uint32_t KIND_TV      = 3;

// A handle for display common unlike those `gsp` gives
static constexpr rm::handle TEST_DISPLAY_COMMON = 0x0005000B;

// A firmware that answers display common's display id controls from its fields, by command and by the display id a
// request names, writing a reply only on `OK`, and records the first `MAX_CALLS` controls sent to it in order
class ids_firmware : public firmware {
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

    uint32_t supported_ids = 0;
    uint32_t connected_ids = 0; // the connect state's reply, whichever display ids its request asks about
    uint32_t retry_time_ms = 0; // the next successful connect reply's retry time, the replies after it giving 0
    uint32_t kinds[DISPLAY_IDS_MAX] = {}; // entry i is the kind a successful request for display id 1 << i reports
    int32_t  supported_result = OK;
    int32_t  connected_result = OK; // the next connect reply's result, the replies after it giving `OK`
    uint32_t kind_failed_ids = 0; // display ids whose kind request returns `kind_result`
    int32_t  kind_result = ERR_FIRMWARE;
    bool     fills_replies = false; // every byte of a reply its fields do not give reads 0xFF

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        record(object, command, params, size);
        if (params == nullptr || size < CONNECT_MASK_OFFSET + sizeof(uint32_t)) {
            stray_count++;
            return OK;
        }

        uint8_t* bytes = static_cast<uint8_t*>(params);
        if (command == NVIDIA_GET_SUPPORTED) {
            return reply(bytes, size, SUPPORTED_MASK_OFFSET, supported_ids, supported_result);
        }

        if (command == NVIDIA_GET_CONNECT_STATE) {
            return reply_connected(bytes, size);
        }

        if (command != NVIDIA_GET_TYPE) {
            stray_count++;
            return OK;
        }

        uint32_t display_id = 0;
        string::memcpy(&display_id, bytes + DISPLAY_ID_OFFSET, sizeof(display_id));
        bool is_single_bit = display_id != 0 && (display_id & (display_id - 1)) == 0;
        if (!is_single_bit) {
            stray_count++;
            return ERR_FIRMWARE;
        }

        uint32_t kind = kinds[__builtin_ctz(display_id)];
        int32_t result = (kind_failed_ids & display_id) != 0 ? kind_result : OK;

        return reply(bytes, size, KIND_OFFSET, kind, result);
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
        recorded = {.object = object, .command = command, .size = size, .has_params = params != nullptr, .sent = {}};
        if (params != nullptr) {
            string::memcpy(recorded.sent, params, size < MAX_BYTES ? size : MAX_BYTES);
        }
    }

    int32_t reply(uint8_t* bytes, size_t size, uint32_t offset, uint32_t value, int32_t result) const {
        if (result != OK) {
            return result;
        }

        if (fills_replies) {
            string::memset(bytes, 0xFF, size);
        }

        string::memcpy(bytes + offset, &value, sizeof(value));

        return OK;
    }

    int32_t reply_connected(uint8_t* bytes, size_t size) {
        int32_t rc = reply(bytes, size, CONNECT_MASK_OFFSET, connected_ids, connected_result);
        connected_result = OK;
        if (rc != OK || size < RETRY_TIME_OFFSET + sizeof(uint32_t)) {
            return rc;
        }

        string::memcpy(bytes + RETRY_TIME_OFFSET, &retry_time_ms, sizeof(retry_time_ms));
        retry_time_ms = 0;

        return OK;
    }
};

// Display ids each `out` holds before a call, so a test sees whether the call wrote it
static constexpr display_ids UNTOUCHED = {.supported = 0xEEEEEEEE, .connected = 0xEEEEEEEE, .digital = 0xEEEEEEEE};

// A report from display common that the read is expected to refuse, with the line logged for it
struct refused_report {
    uint32_t    supported_ids;
    uint32_t    connected_ids;
    const char* logged;
};

// The text logged while `IDS_LOG_BACKEND` is set
static char     s_ids_log[1024] = {};
static uint32_t s_ids_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_ids_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_ids_log_bytes < sizeof(s_ids_log); i++) {
        s_ids_log[s_ids_log_bytes++] = data[i];
    }
}

static constexpr log::backend IDS_LOG_BACKEND = {.write = keep_ids_log};

// Whether the kept text holds `text` anywhere
static bool ids_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_ids_log_bytes; start++) {
        if (string::memcmp(s_ids_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// Reads the display ids with the log kept
static int32_t read_logged(ids_firmware& fw, display_ids& out) {
    s_ids_log_bytes = 0;
    log::set_backend(&IDS_LOG_BACKEND);
    int32_t rc = read_display_ids(fw, TEST_DISPLAY_COMMON, out);
    log::set_backend(nullptr);

    return rc;
}

// Expects `out` to still hold `UNTOUCHED`, field by field
static void expect_untouched(const display_ids& out) {
    EXPECT_EQ(out.supported, UNTOUCHED.supported);
    EXPECT_EQ(out.connected, UNTOUCHED.connected);
    EXPECT_EQ(out.digital, UNTOUCHED.digital);
}

// Expects a recorded call to send `command` to display common with `size` bytes of parameters holding exactly the
// 32-bit fields `expected`
static void expect_request(const ids_firmware::call& sent, uint32_t command, size_t size, const uint32_t* expected) {
    EXPECT_EQ(sent.object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(sent.command, command);
    EXPECT_EQ(sent.size, size);
    EXPECT_TRUE(sent.has_params);
    for (uint32_t field = 0; field < size / sizeof(uint32_t); field++) {
        uint32_t value = 0;
        string::memcpy(&value, sent.sent + field * sizeof(uint32_t), sizeof(value));
        EXPECT_EQ(value, expected[field]);
    }
}

// Requests for the supported display ids, then which are connected, then each supported one's kind in ascending order
// are in NVIDIA's layouts and zeroed but for their display ids, and none follows a successful reply naming a retry time
TEST(nvidia_display_ids, requests_go_to_display_common_in_order_and_nvidias_layouts) {
    ids_firmware earlier;
    earlier.supported_ids = 0x5;
    earlier.connected_ids = 0x5;
    earlier.retry_time_ms = 1;
    earlier.fills_replies = true;

    earlier.kinds[0] = KIND_DFP;
    earlier.kinds[2] = KIND_CRT;

    ids_firmware fw;
    fw.supported_ids = 0x5;
    fw.connected_ids = 0x4;
    fw.fills_replies = true;

    fw.kinds[0] = KIND_DFP;
    fw.kinds[2] = KIND_CRT;

    // Every reply of both reads fills the stack their requests are expected to reuse, so a request not zeroed would
    // likely carry an earlier reply, such as the 1 ms retry time only the earlier read's first connect reply names
    display_ids earlier_ids = {};
    ASSERT_EQ(read_display_ids(earlier, TEST_DISPLAY_COMMON, earlier_ids), OK);
    EXPECT_EQ(earlier.call_count, 4u);

    display_ids ids = {};
    ASSERT_EQ(read_display_ids(fw, TEST_DISPLAY_COMMON, ids), OK);

    ASSERT_EQ(fw.call_count, 4u);
    EXPECT_EQ(fw.stray_count, 0u);

    const uint32_t none[] = {0, 0, 0};
    const uint32_t supported_ids[] = {0, 0, 0x5, 0};
    expect_request(fw.calls[0], NVIDIA_GET_SUPPORTED, NVIDIA_GET_SUPPORTED_BYTES, none);
    expect_request(fw.calls[1], NVIDIA_GET_CONNECT_STATE, NVIDIA_GET_CONNECT_STATE_BYTES, supported_ids);

    const uint32_t first_id[] = {0, 0x1, 0};
    const uint32_t third_id[] = {0, 0x4, 0};
    expect_request(fw.calls[2], NVIDIA_GET_TYPE, NVIDIA_GET_TYPE_BYTES, first_id);
    expect_request(fw.calls[3], NVIDIA_GET_TYPE, NVIDIA_GET_TYPE_BYTES, third_id);
}

// The supported and connected display ids are kept as display common reports them, and with every kind reported
// digital, the digital ones are the supported ones, up to bit 31
TEST(nvidia_display_ids, display_ids_are_kept_as_reported) {
    const display_ids reports[] = {
        {.supported = 0x80000F01, .connected = 0x00000901, .digital = 0x80000F01},
        {.supported = 0x0000FF00, .connected = 0x00000000, .digital = 0x0000FF00},
        {.supported = 0xFFFFFFFF, .connected = 0xFFFFFFFF, .digital = 0xFFFFFFFF},
    };

    for (const display_ids& report : reports) {
        ids_firmware fw;
        fw.supported_ids = report.supported;
        fw.connected_ids = report.connected;
        for (uint32_t bit = 0; bit < DISPLAY_IDS_MAX; bit++) {
            fw.kinds[bit] = KIND_DFP;
        }

        display_ids ids = UNTOUCHED;

        ASSERT_EQ(read_display_ids(fw, TEST_DISPLAY_COMMON, ids), OK);

        EXPECT_EQ(ids.supported, report.supported);
        EXPECT_EQ(ids.connected, report.connected);
        EXPECT_EQ(ids.digital, report.digital);
        EXPECT_EQ(fw.call_count, 2u + __builtin_popcount(report.supported));
    }
}

// The digital display ids are the supported ones display common reports digital
TEST(nvidia_display_ids, digital_ids_are_the_supported_ones_reported_digital) {
    ids_firmware fw;
    display_ids ids = UNTOUCHED;
    fw.supported_ids = 0x80000F01;
    fw.connected_ids = 0x00000901;

    fw.kinds[0] = KIND_CRT;
    fw.kinds[8] = KIND_DFP;
    fw.kinds[9] = KIND_DFP;
    fw.kinds[10] = KIND_TV;
    fw.kinds[11] = KIND_DFP;
    fw.kinds[31] = KIND_DFP;

    ASSERT_EQ(read_display_ids(fw, TEST_DISPLAY_COMMON, ids), OK);

    EXPECT_EQ(ids.supported, 0x80000F01u);
    EXPECT_EQ(ids.connected, 0x00000901u);
    EXPECT_EQ(ids.digital, 0x80000B00u);
    EXPECT_EQ(fw.call_count, 2u + 6u);
}

// A display id whose kind is not digital is logged and left out of the digital ones, whatever kind display common
// reports, even one equal to digital in its low 16 bits
TEST(nvidia_display_ids, display_ids_not_digital_are_logged_and_left_out_of_the_digital_ones) {
    ids_firmware fw;
    display_ids ids = UNTOUCHED;
    fw.supported_ids = 0x1F;

    fw.kinds[0] = KIND_UNKNOWN;
    fw.kinds[1] = KIND_CRT;
    fw.kinds[2] = KIND_TV;
    fw.kinds[3] = 7;
    fw.kinds[4] = 0x10000 | KIND_DFP;

    ASSERT_EQ(read_logged(fw, ids), OK);

    EXPECT_EQ(ids.supported, 0x1Fu);
    EXPECT_EQ(ids.digital, 0u);

    const char* logged[] = {
        "nvidia: display id 0x1 is left out of the digital ones, as display common reports its kind 0",
        "nvidia: display id 0x2 is left out of the digital ones, as display common reports its kind 1",
        "nvidia: display id 0x4 is left out of the digital ones, as display common reports its kind 3",
        "nvidia: display id 0x8 is left out of the digital ones, as display common reports its kind 7",
        "nvidia: display id 0x10 is left out of the digital ones, as display common reports its kind 65538",
    };

    for (const char* line : logged) {
        EXPECT_TRUE(ids_log_contains(line));
    }
}

// A display id whose kind the firmware refuses is logged and left out of the digital ones, and the read goes on with
// the display ids after it
TEST(nvidia_display_ids, refused_kind_leaves_only_that_display_id_out_of_the_digital_ones) {
    ids_firmware fw;
    display_ids ids = UNTOUCHED;
    fw.supported_ids = 0x7;
    fw.connected_ids = 0x7;
    fw.kind_failed_ids = 0x2;

    fw.kinds[0] = KIND_DFP;
    fw.kinds[1] = KIND_DFP;
    fw.kinds[2] = KIND_DFP;

    ASSERT_EQ(read_logged(fw, ids), OK);

    EXPECT_EQ(ids.digital, 0x5u);
    EXPECT_EQ(ids.connected, 0x7u);
    EXPECT_EQ(fw.call_count, 5u);
    EXPECT_TRUE(ids_log_contains(
        "nvidia: display id 0x2 is left out of the digital ones, as display common refused its kind"
    ));
}

// Any other error of a display id's kind request, even after a digital one, ends the read with that error, no later
// request sent and `out` left as it was
TEST(nvidia_display_ids, other_errors_of_a_kind_request_end_the_read) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_BAD_MESSAGE, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        ids_firmware fw;
        display_ids ids = UNTOUCHED;
        fw.supported_ids = 0x7;
        fw.kind_failed_ids = 0x2;
        fw.kind_result = error;

        fw.kinds[0] = KIND_DFP;
        fw.kinds[1] = KIND_DFP;
        fw.kinds[2] = KIND_DFP;

        EXPECT_EQ(read_display_ids(fw, TEST_DISPLAY_COMMON, ids), error);

        EXPECT_EQ(fw.call_count, 4u);
        expect_untouched(ids);
    }
}

// A failed supported or connected report, refused by the firmware or past its deadline, ends the read with its error,
// no later request sent and `out` left as it was
TEST(nvidia_display_ids, failed_supported_or_connected_report_ends_the_read) {
    const int32_t errors[] = {ERR_FIRMWARE, ERR_TIMEOUT};
    for (int32_t error : errors) {
        ids_firmware supported_fails;
        supported_fails.supported_ids = 0x1;
        supported_fails.supported_result = error;

        ids_firmware connected_fails;
        connected_fails.supported_ids = 0x1;
        connected_fails.connected_result = error;

        display_ids from_supported = UNTOUCHED;
        display_ids from_connected = UNTOUCHED;
        EXPECT_EQ(read_display_ids(supported_fails, TEST_DISPLAY_COMMON, from_supported), error);
        EXPECT_EQ(read_display_ids(connected_fails, TEST_DISPLAY_COMMON, from_connected), error);

        EXPECT_EQ(supported_fails.call_count, 1u);
        EXPECT_EQ(connected_fails.call_count, 2u);
        expect_untouched(from_supported);
        expect_untouched(from_connected);
    }
}

// A connected display id outside the supported ones, below their lowest bit, between them or above their highest, is
// refused as malformed and logged, with `out` left as it was
TEST(nvidia_display_ids, connected_display_id_not_supported_is_refused) {
    const refused_report reports[] = {
        {
            .supported_ids = 0x1,
            .connected_ids = 0x3,
            .logged = "nvidia: display common reports display ids 0x3 connected, some of them outside the supported "
                      "0x1",
        },
        {
            .supported_ids = 0xFF00,
            .connected_ids = 0x0101,
            .logged = "nvidia: display common reports display ids 0x101 connected, some of them outside the supported "
                      "0xFF00",
        },
        {
            .supported_ids = 0xFF00,
            .connected_ids = 0x80000100,
            .logged = "nvidia: display common reports display ids 0x80000100 connected, some of them outside the "
                      "supported 0xFF00",
        },
        {
            .supported_ids = 0x0F0F,
            .connected_ids = 0x0010,
            .logged = "nvidia: display common reports display ids 0x10 connected, some of them outside the supported "
                      "0xF0F",
        },
    };

    for (const refused_report& report : reports) {
        ids_firmware fw;
        fw.supported_ids = report.supported_ids;
        fw.connected_ids = report.connected_ids;
        display_ids ids = UNTOUCHED;

        EXPECT_EQ(read_logged(fw, ids), ERR_BAD_MESSAGE);

        EXPECT_EQ(fw.call_count, 2u);
        EXPECT_TRUE(ids_log_contains(report.logged));
        expect_untouched(ids);
    }
}

// With no display id supported the read still asks the connect state, about no display id, and gives none
TEST(nvidia_display_ids, no_supported_display_id_gives_none) {
    ids_firmware fw;
    display_ids ids = UNTOUCHED;

    ASSERT_EQ(read_display_ids(fw, TEST_DISPLAY_COMMON, ids), OK);

    ASSERT_EQ(fw.call_count, 2u);
    const uint32_t no_display_id[] = {0, 0, 0, 0};
    expect_request(fw.calls[1], NVIDIA_GET_CONNECT_STATE, NVIDIA_GET_CONNECT_STATE_BYTES, no_display_id);
    EXPECT_EQ(ids.supported, 0u);
    EXPECT_EQ(ids.connected, 0u);
    EXPECT_EQ(ids.digital, 0u);
}
