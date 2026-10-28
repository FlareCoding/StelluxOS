// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_dpcd);

using drivers::nvidia::DPCD_EXTENDED_COPY_PRESENT;
using drivers::nvidia::DPCD_EXTENDED_RECEIVER_CAPABILITIES;
using drivers::nvidia::DPCD_PHY_REPEATER_CAPABILITIES;
using drivers::nvidia::DPCD_PHY_REPEATER_CAPABILITY_BYTES;
using drivers::nvidia::DPCD_RECEIVER_CAPABILITIES;
using drivers::nvidia::DPCD_RECEIVER_CAPABILITY_BYTES;
using drivers::nvidia::DPCD_TRAINING_AUX_READ_INTERVAL;
using drivers::nvidia::dpcd_capabilities;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::read_output_dpcd;
namespace rm = drivers::nvidia::rm;

// NVIDIA's DP_AUXCH_CTRL, its parameters' size, the offsets of the fields sent or read in them, the data field's size,
// the command of a native read and the reply types, restated so the tests hold the requests to NVIDIA's layout
static constexpr uint32_t NVIDIA_DP_AUXCH_CTRL       = 0x731341;
static constexpr size_t   NVIDIA_DP_AUXCH_CTRL_BYTES = 48;
static constexpr uint32_t DISPLAY_ID_OFFSET          = 4;
static constexpr uint32_t COMMAND_OFFSET             = 12;
static constexpr uint32_t ADDRESS_OFFSET             = 16;
static constexpr uint32_t DATA_OFFSET                = 20;
static constexpr uint32_t DATA_BYTES                 = 16;
static constexpr uint32_t SIZE_OFFSET                = 36;
static constexpr uint32_t REPLY_OFFSET               = 40;
static constexpr uint32_t NVIDIA_NATIVE_READ         = 0x9;
static constexpr uint32_t NVIDIA_REPLY_ACK           = 0;
static constexpr uint32_t NVIDIA_REPLY_NACK          = 1;
static constexpr uint32_t NVIDIA_REPLY_DEFER         = 2;
static constexpr uint32_t NVIDIA_REPLY_TIMEOUT       = 3;

// NVIDIA's DPCD addresses of the byte read first, the receiver capability field, its extended copy and the PHY
// repeaters' capabilities, the bit saying the extended copy is present, and the bytes NVIDIA reads of each field
static constexpr uint32_t NVIDIA_TRAINING_AUX_RD_INTERVAL      = 0x0000E;
static constexpr uint32_t NVIDIA_DPCD_REV                      = 0x00000;
static constexpr uint32_t NVIDIA_EXTENDED_REV                  = 0x02200;
static constexpr uint32_t NVIDIA_LT_TUNABLE_PHY_REPEATER_REV   = 0xF0000;
static constexpr uint8_t  NVIDIA_EXTENDED_RX_CAP               = 0x80;
static constexpr uint32_t NVIDIA_RECEIVER_CAPABILITY_BYTES     = 16;
static constexpr uint32_t NVIDIA_PHY_REPEATER_CAPABILITY_BYTES = 8;

// A handle for display common unlike those `gsp` gives, and an output's display id
static constexpr rm::handle TEST_DISPLAY_COMMON = 0x0005000B;
static constexpr uint32_t   TEST_DISPLAY_ID     = 0x100;

// A DisplayPort 1.4 monitor's receiver capability field, revision 1.2 with 5.4 Gbps at most for older sources and 4
// lanes, its byte 14 saying the extended copy is present
static constexpr uint8_t MONITOR_RECEIVER_CAPABILITIES[DATA_BYTES] = {
    0x12, 0x14, 0xC4, 0x81, 0x01, 0x00, 0x01, 0xC0, 0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x84, 0x00,
};

// The same monitor's extended copy, revision 1.4 with 8.1 Gbps at most
static constexpr uint8_t MONITOR_EXTENDED_RECEIVER_CAPABILITIES[DATA_BYTES] = {
    0x14, 0x1E, 0xC4, 0x81, 0x01, 0x00, 0x01, 0xC0, 0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x84, 0x00,
};

// A DisplayPort 1.2 monitor's receiver capability field, whose byte 14 has every bit but the extended copy's set
static constexpr uint8_t BASE_ONLY_RECEIVER_CAPABILITIES[DATA_BYTES] = {
    0x12, 0x14, 0xC4, 0x81, 0x01, 0x00, 0x01, 0xC0, 0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x7F, 0x00,
};

// The capabilities of the PHY repeaters between the card and the monitor: revision 1.4, 8.1 Gbps at most, one repeater
// and 4 lanes
static constexpr uint8_t PHY_REPEATER_CAPABILITIES[NVIDIA_PHY_REPEATER_CAPABILITY_BYTES] = {
    0x14, 0x1E, 0x80, 0x00, 0x04, 0x00, 0x00, 0x00,
};

// A firmware that answers DP_AUXCH_CTRL's native reads from a DPCD holding `receiver`, `extended_receiver` and
// `phy_repeaters` at their addresses and 0 elsewhere, and records the first `MAX_CALLS` controls sent to it
class dpcd_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 4;

    struct call {
        rm::handle object;
        uint32_t   command;
        size_t     size;
        uint32_t   display_id;
        uint32_t   aux_command;
        uint32_t   address;
        uint32_t   size_field;
        bool       is_zero_but_sent; // every byte but the display id's, command's, address's and size's is 0
    };

    call     calls[MAX_CALLS] = {};
    uint32_t call_count = 0;
    uint32_t stray_count = 0;

    const uint8_t* receiver = MONITOR_RECEIVER_CAPABILITIES;
    const uint8_t* extended_receiver = MONITOR_EXTENDED_RECEIVER_CAPABILITIES;
    const uint8_t* phy_repeaters = PHY_REPEATER_CAPABILITIES;

    // Returned with no reply from call `failing_call` on, unless `OK`
    int32_t  result = OK;
    uint32_t failing_call = 1;

    // The reply type and reply size of call `scripted_call`, unless it is 0
    uint32_t scripted_call = 0;
    uint32_t scripted_reply = NVIDIA_REPLY_ACK;
    uint32_t scripted_size = 0;

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        record(object, command, params, size);

        bool is_aux_control = command == NVIDIA_DP_AUXCH_CTRL;
        bool has_aux_params = params != nullptr && size == NVIDIA_DP_AUXCH_CTRL_BYTES;
        if (!is_aux_control || !has_aux_params) {
            stray_count++;
            return OK;
        }

        if (result != OK && call_count >= failing_call) {
            return result;
        }

        uint8_t* bytes = static_cast<uint8_t*>(params);
        uint32_t address = field_at(bytes, ADDRESS_OFFSET);
        uint32_t reply_size = field_at(bytes, SIZE_OFFSET) + 1;
        uint32_t reply = NVIDIA_REPLY_ACK;
        if (call_count == scripted_call) {
            reply_size = scripted_size;
            reply = scripted_reply;
        }

        // Every byte the reply does not give is dirtied, so a later request reusing this stack shows if not zeroed
        string::memset(bytes, 0xA5, size);
        for (uint32_t i = 0; i < reply_size && i < DATA_BYTES; i++) {
            bytes[DATA_OFFSET + i] = byte_at(address + i);
        }

        string::memcpy(bytes + SIZE_OFFSET, &reply_size, sizeof(uint32_t));
        string::memcpy(bytes + REPLY_OFFSET, &reply, sizeof(uint32_t));

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
    // The 32-bit field at `offset` of the parameters
    static uint32_t field_at(const uint8_t* bytes, uint32_t offset) {
        uint32_t value = 0;
        string::memcpy(&value, bytes + offset, sizeof(uint32_t));
        return value;
    }

    // The DPCD's byte at `address`
    uint8_t byte_at(uint32_t address) const {
        if (address < NVIDIA_DPCD_REV + NVIDIA_RECEIVER_CAPABILITY_BYTES) {
            return receiver[address - NVIDIA_DPCD_REV];
        }

        uint32_t extended_end = NVIDIA_EXTENDED_REV + NVIDIA_RECEIVER_CAPABILITY_BYTES;
        if (address >= NVIDIA_EXTENDED_REV && address < extended_end) {
            return extended_receiver[address - NVIDIA_EXTENDED_REV];
        }

        uint32_t repeaters_end = NVIDIA_LT_TUNABLE_PHY_REPEATER_REV + NVIDIA_PHY_REPEATER_CAPABILITY_BYTES;
        if (address >= NVIDIA_LT_TUNABLE_PHY_REPEATER_REV && address < repeaters_end) {
            return phy_repeaters[address - NVIDIA_LT_TUNABLE_PHY_REPEATER_REV];
        }

        return 0;
    }

    // Counts a control and keeps the first `MAX_CALLS` with the fields an AUX request sends
    void record(rm::handle object, uint32_t command, void* params, size_t size) {
        call_count++;
        if (call_count > MAX_CALLS) {
            return;
        }

        call& recorded = calls[call_count - 1];
        recorded.object = object;
        recorded.command = command;
        recorded.size = size;
        if (params == nullptr || size != NVIDIA_DP_AUXCH_CTRL_BYTES) {
            return;
        }

        const uint8_t* bytes = static_cast<const uint8_t*>(params);
        recorded.display_id = field_at(bytes, DISPLAY_ID_OFFSET);
        recorded.aux_command = field_at(bytes, COMMAND_OFFSET);
        recorded.address = field_at(bytes, ADDRESS_OFFSET);
        recorded.size_field = field_at(bytes, SIZE_OFFSET);

        recorded.is_zero_but_sent = true;
        for (size_t i = 0; i < size; i++) {
            bool is_sent = is_in_field(i, DISPLAY_ID_OFFSET)
                || is_in_field(i, COMMAND_OFFSET)
                || is_in_field(i, ADDRESS_OFFSET)
                || is_in_field(i, SIZE_OFFSET);
            if (!is_sent && bytes[i] != 0) {
                recorded.is_zero_but_sent = false;
            }
        }
    }

    // Whether byte `index` of the parameters lies in the 32-bit field at `offset`
    static bool is_in_field(size_t index, uint32_t offset) {
        return index >= offset && index < offset + sizeof(uint32_t);
    }
};

// Each read of the monitor with an extended copy, in order, with its address, the size field it sends and how the log
// names it
struct expected_read {
    uint32_t    address;
    uint32_t    size_field;
    const char* logged;
};

static constexpr expected_read EXTENDED_READS[] = {
    {NVIDIA_TRAINING_AUX_RD_INTERVAL, 0, "1-byte AUX read at 0x0000E"},
    {NVIDIA_EXTENDED_REV, NVIDIA_RECEIVER_CAPABILITY_BYTES - 1, "16-byte AUX read at 0x02200"},
    {NVIDIA_LT_TUNABLE_PHY_REPEATER_REV, NVIDIA_PHY_REPEATER_CAPABILITY_BYTES - 1, "8-byte AUX read at 0xF0000"},
};

// The byte every byte of a test's capabilities holds before a read, so a test sees whether the read wrote them
static constexpr uint8_t UNTOUCHED = 0xEE;

// How each line logged for a read that cannot be made ends
static constexpr const char* NO_MORE_READ = ", so no more of its DPCD is read";

// Whether every byte of `capabilities` still holds `UNTOUCHED`
static bool capabilities_untouched(const dpcd_capabilities& capabilities) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&capabilities);
    for (size_t i = 0; i < sizeof(capabilities); i++) {
        if (bytes[i] != UNTOUCHED) {
            return false;
        }
    }

    return true;
}

// The text logged while `DPCD_LOG_BACKEND` is set
static char     s_dpcd_log[2048] = {};
static uint32_t s_dpcd_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_dpcd_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_dpcd_log_bytes < sizeof(s_dpcd_log); i++) {
        s_dpcd_log[s_dpcd_log_bytes++] = data[i];
    }
}

static constexpr log::backend DPCD_LOG_BACKEND = {.write = keep_dpcd_log};

// Whether the kept text holds `text` anywhere
static bool dpcd_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_dpcd_log_bytes; start++) {
        if (string::memcmp(s_dpcd_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// Reads the test display id's capabilities into `out` with the log kept
static int32_t read_logged(dpcd_firmware& fw, dpcd_capabilities& out, bool& is_read) {
    s_dpcd_log_bytes = 0;
    log::set_backend(&DPCD_LOG_BACKEND);
    int32_t rc = read_output_dpcd(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, out, is_read);
    log::set_backend(nullptr);

    return rc;
}

// The reads go to display common in NVIDIA's command and size as native reads of the byte at 0x0000E, then the extended
// copy's 16 bytes, then the PHY repeaters' 8 bytes, each zeroed but for the fields it sends
TEST(nvidia_display_dpcd, reads_go_to_display_common_in_nvidias_layout_and_order) {
    dpcd_firmware earlier;
    dpcd_capabilities earlier_out = {};
    bool is_read = false;

    dpcd_firmware fw;
    dpcd_capabilities out = {};

    // The earlier read fills the stack the next read's requests are expected to reuse, so a request not zeroed would
    // likely carry its replies
    ASSERT_EQ(read_output_dpcd(earlier, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, earlier_out, is_read), OK);
    ASSERT_EQ(read_output_dpcd(fw, TEST_DISPLAY_COMMON, 0x800, out, is_read), OK);

    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_EQ(fw.stray_count, 0u);
    for (uint32_t index = 0; index < 3; index++) {
        EXPECT_EQ(fw.calls[index].object, TEST_DISPLAY_COMMON);
        EXPECT_EQ(fw.calls[index].command, NVIDIA_DP_AUXCH_CTRL);
        EXPECT_EQ(fw.calls[index].size, NVIDIA_DP_AUXCH_CTRL_BYTES);

        EXPECT_EQ(fw.calls[index].display_id, 0x800u);
        EXPECT_EQ(fw.calls[index].aux_command, NVIDIA_NATIVE_READ);
        EXPECT_EQ(fw.calls[index].address, EXTENDED_READS[index].address);
        EXPECT_EQ(fw.calls[index].size_field, EXTENDED_READS[index].size_field);
        EXPECT_TRUE(fw.calls[index].is_zero_but_sent);
    }
}

// A monitor whose byte at 0x0000E has bit 7 set gives the extended copy and the PHY repeaters' bytes as read, with
// `is_read` set and nothing logged
TEST(nvidia_display_dpcd, an_extended_copy_is_read_when_its_bit_is_set) {
    dpcd_firmware fw;
    dpcd_capabilities out = {};
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read), OK);

    EXPECT_TRUE(is_read);
    EXPECT_EQ(s_dpcd_log_bytes, 0u);
    EXPECT_TRUE(out.is_extended);
    EXPECT_EQ(string::memcmp(out.receiver, MONITOR_EXTENDED_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
    EXPECT_EQ(string::memcmp(out.phy_repeaters, PHY_REPEATER_CAPABILITIES, sizeof(PHY_REPEATER_CAPABILITIES)), 0);
}

// A monitor whose byte at 0x0000E has every bit but bit 7 set gives the field from 0x00000
TEST(nvidia_display_dpcd, the_field_is_read_from_0_when_the_bit_is_clear) {
    dpcd_firmware fw;
    fw.receiver = BASE_ONLY_RECEIVER_CAPABILITIES;
    dpcd_capabilities out = {};
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read), OK);

    EXPECT_TRUE(is_read);
    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_EQ(fw.calls[1].address, NVIDIA_DPCD_REV);
    EXPECT_EQ(fw.calls[1].size_field, NVIDIA_RECEIVER_CAPABILITY_BYTES - 1);

    EXPECT_FALSE(out.is_extended);
    EXPECT_EQ(string::memcmp(out.receiver, BASE_ONLY_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
    EXPECT_EQ(string::memcmp(out.phy_repeaters, PHY_REPEATER_CAPABILITIES, sizeof(PHY_REPEATER_CAPABILITIES)), 0);
}

// A read the firmware refuses is logged and ends the reads with `is_read` cleared, no later read and `out` as it was
TEST(nvidia_display_dpcd, a_refused_read_is_logged_and_ends_the_reads) {
    for (uint32_t index = 0; index < 3; index++) {
        dpcd_firmware fw;
        fw.result = ERR_FIRMWARE;
        fw.failing_call = index + 1;
        dpcd_capabilities out;
        string::memset(&out, UNTOUCHED, sizeof(out));
        bool is_read = true;

        ASSERT_EQ(read_logged(fw, out, is_read), OK);

        EXPECT_FALSE(is_read);
        EXPECT_EQ(fw.call_count, index + 1);
        EXPECT_TRUE(capabilities_untouched(out));
        EXPECT_TRUE(dpcd_log_contains("nvidia: display common refused display id 0x100's "));
        EXPECT_TRUE(dpcd_log_contains(EXTENDED_READS[index].logged));
        EXPECT_TRUE(dpcd_log_contains(NO_MORE_READ));
    }
}

// Any other error of a read ends the reads with that error, with no later read, nothing logged and `out` as it was
TEST(nvidia_display_dpcd, other_errors_of_a_read_end_the_reads) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_BAD_MESSAGE, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        for (uint32_t index = 0; index < 3; index++) {
            dpcd_firmware fw;
            fw.result = error;
            fw.failing_call = index + 1;
            dpcd_capabilities out;
            string::memset(&out, UNTOUCHED, sizeof(out));
            bool is_read = true;

            EXPECT_EQ(read_logged(fw, out, is_read), error);

            EXPECT_FALSE(is_read);
            EXPECT_EQ(fw.call_count, index + 1);
            EXPECT_TRUE(capabilities_untouched(out));
            EXPECT_EQ(s_dpcd_log_bytes, 0u);
        }
    }
}

// A reply other than an acknowledgment, a deferred one and one whose low half alone is an acknowledgment included, is
// logged with its meaning and ends the reads with `is_read` cleared, no later read and `out` as it was
TEST(nvidia_display_dpcd, a_reply_other_than_an_acknowledgment_is_logged_and_ends_the_reads) {
    struct other_reply {
        uint32_t    reply;
        const char* logged;
    };

    const other_reply replies[] = {
        {NVIDIA_REPLY_NACK, "ended with a refusal from its monitor (reply type 1)"},
        {NVIDIA_REPLY_DEFER, "ended with a request to ask again later from its monitor (reply type 2)"},
        {NVIDIA_REPLY_TIMEOUT, "ended with no answer in time from its monitor (reply type 3)"},
        {5, "ended with an unknown reply from its monitor (reply type 5)"},
        {0x10000, "ended with an unknown reply from its monitor (reply type 65536)"},
    };

    for (const other_reply& reply : replies) {
        for (uint32_t index = 0; index < 3; index++) {
            // The reply moves no bytes, so a size check made before the reply type's would log another line
            dpcd_firmware fw;
            fw.scripted_call = index + 1;
            fw.scripted_reply = reply.reply;
            fw.scripted_size = 0;

            dpcd_capabilities out;
            string::memset(&out, UNTOUCHED, sizeof(out));
            bool is_read = true;

            ASSERT_EQ(read_logged(fw, out, is_read), OK);

            EXPECT_FALSE(is_read);
            EXPECT_EQ(fw.call_count, index + 1);
            EXPECT_TRUE(capabilities_untouched(out));

            EXPECT_TRUE(dpcd_log_contains("nvidia: display id 0x100's "));
            EXPECT_TRUE(dpcd_log_contains(EXTENDED_READS[index].logged));
            EXPECT_TRUE(dpcd_log_contains(reply.logged));
            EXPECT_TRUE(dpcd_log_contains(NO_MORE_READ));
        }
    }
}

// An acknowledgment moving other than the bytes asked for, none, fewer, more or a count whose low half alone is right,
// is logged with its reply size and ends the reads with `is_read` cleared, no later read and `out` as it was
TEST(nvidia_display_dpcd, an_acknowledgment_of_other_than_the_bytes_asked_for_is_logged_and_ends_the_reads) {
    struct sized_reply {
        uint32_t    call;
        uint32_t    reply_size;
        const char* logged;
    };

    // Static, since the compiler fills a local table this large by calling `memcpy`, which the kernel does not define
    static constexpr sized_reply replies[] = {
        {1, 0, "1-byte AUX read at 0x0000E with a reply size of 0"},
        {1, 2, "1-byte AUX read at 0x0000E with a reply size of 2"},
        {2, 0, "16-byte AUX read at 0x02200 with a reply size of 0"},
        {2, 15, "16-byte AUX read at 0x02200 with a reply size of 15"},
        {2, 17, "16-byte AUX read at 0x02200 with a reply size of 17"},
        {2, 0x10010, "16-byte AUX read at 0x02200 with a reply size of 65552"},
        {3, 0, "8-byte AUX read at 0xF0000 with a reply size of 0"},
        {3, 7, "8-byte AUX read at 0xF0000 with a reply size of 7"},
        {3, 9, "8-byte AUX read at 0xF0000 with a reply size of 9"},
    };

    for (const sized_reply& reply : replies) {
        dpcd_firmware fw;
        fw.scripted_call = reply.call;
        fw.scripted_size = reply.reply_size;
        dpcd_capabilities out;
        string::memset(&out, UNTOUCHED, sizeof(out));
        bool is_read = true;

        ASSERT_EQ(read_logged(fw, out, is_read), OK);

        EXPECT_FALSE(is_read);
        EXPECT_EQ(fw.call_count, reply.call);
        EXPECT_TRUE(capabilities_untouched(out));
        EXPECT_TRUE(dpcd_log_contains("nvidia: display id 0x100's monitor acknowledged the "));
        EXPECT_TRUE(dpcd_log_contains(reply.logged));
        EXPECT_TRUE(dpcd_log_contains(NO_MORE_READ));
    }
}

// The AUX control, its parameters' size, the most bytes a transaction carries, the read command, the reply types and
// the DPCD addresses, bit and lengths are NVIDIA's
TEST(nvidia_display_dpcd, aux_and_dpcd_constants_are_nvidias) {
    EXPECT_EQ(rm::CONTROL_DP_AUX, NVIDIA_DP_AUXCH_CTRL);
    EXPECT_EQ(sizeof(rm::aux_params), NVIDIA_DP_AUXCH_CTRL_BYTES);
    EXPECT_EQ(rm::AUX_DATA_MAX_BYTES, DATA_BYTES);
    EXPECT_EQ(rm::AUX_COMMAND_DPCD_READ, NVIDIA_NATIVE_READ);

    EXPECT_EQ(rm::AUX_REPLY_ACK, NVIDIA_REPLY_ACK);
    EXPECT_EQ(rm::AUX_REPLY_NACK, NVIDIA_REPLY_NACK);
    EXPECT_EQ(rm::AUX_REPLY_DEFER, NVIDIA_REPLY_DEFER);
    EXPECT_EQ(rm::AUX_REPLY_TIMEOUT, NVIDIA_REPLY_TIMEOUT);

    EXPECT_EQ(DPCD_TRAINING_AUX_READ_INTERVAL, NVIDIA_TRAINING_AUX_RD_INTERVAL);
    EXPECT_EQ(DPCD_RECEIVER_CAPABILITIES, NVIDIA_DPCD_REV);
    EXPECT_EQ(DPCD_EXTENDED_RECEIVER_CAPABILITIES, NVIDIA_EXTENDED_REV);
    EXPECT_EQ(DPCD_PHY_REPEATER_CAPABILITIES, NVIDIA_LT_TUNABLE_PHY_REPEATER_REV);

    EXPECT_EQ(DPCD_EXTENDED_COPY_PRESENT, NVIDIA_EXTENDED_RX_CAP);
    EXPECT_EQ(DPCD_RECEIVER_CAPABILITY_BYTES, NVIDIA_RECEIVER_CAPABILITY_BYTES);
    EXPECT_EQ(DPCD_PHY_REPEATER_CAPABILITY_BYTES, NVIDIA_PHY_REPEATER_CAPABILITY_BYTES);
}
