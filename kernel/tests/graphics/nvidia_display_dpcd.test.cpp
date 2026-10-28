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
using drivers::nvidia::take_displayport_from_firmware;
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
static constexpr uint32_t NVIDIA_REPLY_I2CNACK       = 4;
static constexpr uint32_t NVIDIA_REPLY_I2CDEFER      = 8;

// NVIDIA's DP_SET_MANUAL_DISPLAYPORT and its parameters' size, restated to hold its request to NVIDIA's layout
static constexpr uint32_t NVIDIA_DP_SET_MANUAL_DISPLAYPORT       = 0x731365;
static constexpr size_t   NVIDIA_DP_SET_MANUAL_DISPLAYPORT_BYTES = 4;

// NVIDIA's library's requests for the byte at 0x0000E (minimumRetriesOnDefer), for the field and for the PHY
// repeaters' bytes, and its passes, the first and the 3 more (faultyRetries) it makes while the field cannot be read
static constexpr uint32_t NVIDIA_INTERVAL_REQUESTS   = 7;
static constexpr uint32_t NVIDIA_CAPABILITY_REQUESTS = 16;
static constexpr uint32_t NVIDIA_PASSES              = 4;

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

// The addresses the reads use: the byte at 0x0000E, the field, its extended copy and the PHY repeaters' bytes
static constexpr uint32_t READ_ADDRESSES[] = {
    NVIDIA_TRAINING_AUX_RD_INTERVAL,
    NVIDIA_DPCD_REV,
    NVIDIA_EXTENDED_REV,
    NVIDIA_LT_TUNABLE_PHY_REPEATER_REV,
};

static constexpr uint32_t READ_ADDRESS_COUNT = sizeof(READ_ADDRESSES) / sizeof(READ_ADDRESSES[0]);

// A `scripted_requests` that scripts every request at its address
static constexpr uint32_t EVERY_REQUEST = 0xFFFFFFFF;

// A firmware that answers DP_AUXCH_CTRL's native reads from a DPCD holding `receiver`, `extended_receiver` and
// `phy_repeaters` at their addresses and 0 elsewhere, counts its requests and records the first `MAX_CALLS` controls
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

    // The requests made at each of `READ_ADDRESSES`
    uint32_t requests[READ_ADDRESS_COUNT] = {};

    // Returned with no reply from call `failing_call` on, unless `OK`
    int32_t  result = OK;
    uint32_t failing_call = 1;

    // The first `scripted_requests` requests at `scripted_address` return `scripted_result` with no reply, or when it
    // is `OK`, end with `scripted_reply` and the reply size `scripted_size`
    uint32_t scripted_address = 0;
    uint32_t scripted_requests = 0;
    int32_t  scripted_result = OK;
    uint32_t scripted_reply = NVIDIA_REPLY_ACK;
    uint32_t scripted_size = 0;

    // The `later_requests` requests there after those end with `later_reply` and the reply size `later_size`
    uint32_t later_requests = 0;
    uint32_t later_reply = NVIDIA_REPLY_ACK;
    uint32_t later_size = 0;

    // What DP_SET_MANUAL_DISPLAYPORT returns, and the subdevice its last request named
    int32_t  manual_result = OK;
    uint32_t manual_subdevice = 0xFFFFFFFF;

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        record(object, command, params, size);

        bool is_manual_control = command == NVIDIA_DP_SET_MANUAL_DISPLAYPORT;
        bool has_manual_params = params != nullptr && size == NVIDIA_DP_SET_MANUAL_DISPLAYPORT_BYTES;
        if (is_manual_control && has_manual_params) {
            manual_subdevice = field_at(static_cast<uint8_t*>(params), 0);
            return manual_result;
        }

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
        uint32_t requests_at_address = count_request(address);
        bool is_at_script = address == scripted_address;
        bool is_scripted = is_at_script && requests_at_address <= scripted_requests;
        if (is_scripted && scripted_result != OK) {
            return scripted_result;
        }

        bool is_later = is_at_script && !is_scripted && requests_at_address - scripted_requests <= later_requests;
        uint32_t reply_size = field_at(bytes, SIZE_OFFSET) + 1;
        uint32_t reply = NVIDIA_REPLY_ACK;
        if (is_scripted) {
            reply_size = scripted_size;
            reply = scripted_reply;
        } else if (is_later) {
            reply_size = later_size;
            reply = later_reply;
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

    // The requests made at `address`, 0 for an address no read uses
    uint32_t requests_at(uint32_t address) const {
        for (uint32_t slot = 0; slot < READ_ADDRESS_COUNT; slot++) {
            if (READ_ADDRESSES[slot] == address) {
                return requests[slot];
            }
        }

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

    // Counts a request at `address`, one at an address no read uses as stray, and gives the requests made at that
    // address so far, or every stray request so far for a stray one, this one included
    uint32_t count_request(uint32_t address) {
        for (uint32_t slot = 0; slot < READ_ADDRESS_COUNT; slot++) {
            if (READ_ADDRESSES[slot] == address) {
                requests[slot]++;
                return requests[slot];
            }
        }

        stray_count++;

        return stray_count;
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

// An answer every request at one address gets, whether the read is asked again on it, and how the log names it
struct failing_answer {
    int32_t     result; // `OK`, or `ERR_FIRMWARE` for the firmware's refusal
    uint32_t    reply;
    uint32_t    reply_size;
    bool        is_asked_again;
    const char* logged;
};

// A read at `address`, the requests bounding it, and the reply type and reply size its first requests all get
struct asked_again_read {
    uint32_t address;
    uint32_t requests;
    uint32_t reply;
    uint32_t reply_size;
};

// The byte every byte of a test's capabilities holds before a read, so a test sees whether the read wrote them
static constexpr uint8_t UNTOUCHED = 0xEE;

// How the line logged for a read that gives no bytes ends: for the byte at 0x0000E, for the field in a pass but the
// last and in the last, and for the PHY repeaters' bytes
static constexpr const char* FIELD_FROM_BASE = ", so its receiver capability field is read from 0x00000";
static constexpr const char* READ_AGAIN      = ", so its DPCD is read again";
static constexpr const char* NO_MORE_READ    = ", so no more of its DPCD is read";
static constexpr const char* NO_PHY_REPEATER = ", so its link is taken to have no PHY repeater";

// How the log names a read whose one request the monitor answers with reply type 4, NVIDIA's I2CNACK
static constexpr const char* I2C_REFUSAL_LOGGED =
    "a refusal of an I2C transaction from its monitor (reply type 4) after 1 request,";

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

// How many times the kept text holds `text`
static uint32_t dpcd_log_count(const char* text) {
    uint32_t count = 0;
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_dpcd_log_bytes; start++) {
        if (string::memcmp(s_dpcd_log + start, text, text_bytes) == 0) {
            count++;
        }
    }

    return count;
}

// Whether the kept text holds `text` anywhere
static bool dpcd_log_contains(const char* text) {
    return dpcd_log_count(text) != 0;
}

// Reads the test display id's capabilities into `out` with the log kept, for a GPU training PHY repeaters unless
// `trains_phy_repeaters` is cleared
static int32_t read_logged(
    dpcd_firmware& fw,
    dpcd_capabilities& out,
    bool& is_read,
    bool trains_phy_repeaters = true
) {
    s_dpcd_log_bytes = 0;
    log::set_backend(&DPCD_LOG_BACKEND);
    int32_t rc = read_output_dpcd(fw, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, trains_phy_repeaters, out, is_read);
    log::set_backend(nullptr);

    return rc;
}

// Takes the DisplayPort hardware from `fw` with the log kept
static int32_t take_logged(dpcd_firmware& fw) {
    s_dpcd_log_bytes = 0;
    log::set_backend(&DPCD_LOG_BACKEND);
    int32_t rc = take_displayport_from_firmware(fw, TEST_DISPLAY_COMMON);
    log::set_backend(nullptr);

    return rc;
}

// Has every request at `address` get `answer`
static void script_every_request(dpcd_firmware& fw, uint32_t address, const failing_answer& answer) {
    fw.scripted_address = address;
    fw.scripted_requests = EVERY_REQUEST;
    fw.scripted_result = answer.result;
    fw.scripted_reply = answer.reply;
    fw.scripted_size = answer.reply_size;
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
    ASSERT_EQ(read_output_dpcd(earlier, TEST_DISPLAY_COMMON, TEST_DISPLAY_ID, true, earlier_out, is_read), OK);
    ASSERT_EQ(read_output_dpcd(fw, TEST_DISPLAY_COMMON, 0x800, true, out, is_read), OK);

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
// `is_read` and `has_phy_repeater_bytes` set and nothing logged
TEST(nvidia_display_dpcd, an_extended_copy_is_read_when_its_bit_is_set) {
    dpcd_firmware fw;
    dpcd_capabilities out = {};
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read), OK);

    EXPECT_TRUE(is_read);
    EXPECT_EQ(s_dpcd_log_bytes, 0u);
    EXPECT_TRUE(out.is_extended);
    EXPECT_EQ(string::memcmp(out.receiver, MONITOR_EXTENDED_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
    EXPECT_TRUE(out.has_phy_repeater_bytes);
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

// For a GPU that trains no PHY repeaters only the byte at 0x0000E and the field are read, with `is_read` set,
// `has_phy_repeater_bytes` cleared, the PHY repeaters' bytes 0 and nothing logged
TEST(nvidia_display_dpcd, phy_repeaters_are_not_read_when_the_gpu_trains_none) {
    dpcd_firmware fw;
    dpcd_capabilities out;
    string::memset(&out, UNTOUCHED, sizeof(out));
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read, false), OK);

    EXPECT_TRUE(is_read);
    EXPECT_EQ(s_dpcd_log_bytes, 0u);
    ASSERT_EQ(fw.call_count, 2u);
    EXPECT_EQ(fw.calls[0].address, NVIDIA_TRAINING_AUX_RD_INTERVAL);
    EXPECT_EQ(fw.calls[1].address, NVIDIA_EXTENDED_REV);

    EXPECT_TRUE(out.is_extended);
    EXPECT_EQ(string::memcmp(out.receiver, MONITOR_EXTENDED_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
    EXPECT_FALSE(out.has_phy_repeater_bytes);
    for (uint8_t byte : out.phy_repeaters) {
        EXPECT_EQ(byte, 0u);
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

static constexpr asked_again_read ASKED_AGAIN_READS[] = {
    {NVIDIA_TRAINING_AUX_RD_INTERVAL, NVIDIA_INTERVAL_REQUESTS, NVIDIA_REPLY_DEFER, 0},
    {NVIDIA_TRAINING_AUX_RD_INTERVAL, NVIDIA_INTERVAL_REQUESTS, NVIDIA_REPLY_I2CDEFER, 0},
    {NVIDIA_TRAINING_AUX_RD_INTERVAL, NVIDIA_INTERVAL_REQUESTS, NVIDIA_REPLY_ACK, 2},
    {NVIDIA_EXTENDED_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_DEFER, 0},
    {NVIDIA_EXTENDED_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_I2CDEFER, 0},
    {NVIDIA_EXTENDED_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_ACK, 15},
    {NVIDIA_EXTENDED_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_ACK, 17},
    {NVIDIA_EXTENDED_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_ACK, 0x10010},
    {NVIDIA_LT_TUNABLE_PHY_REPEATER_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_DEFER, 0},
    {NVIDIA_LT_TUNABLE_PHY_REPEATER_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_I2CDEFER, 0},
    {NVIDIA_LT_TUNABLE_PHY_REPEATER_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_ACK, 7},
    {NVIDIA_LT_TUNABLE_PHY_REPEATER_REV, NVIDIA_CAPABILITY_REQUESTS, NVIDIA_REPLY_ACK, 9},
};

// A read the monitor defers (reply type 2), answers with an I2C deferral (reply type 8) or acknowledges with some but
// not all of the bytes or more, a size right in its low half only included, is asked again and read at its last request
TEST(nvidia_display_dpcd, a_deferred_or_wrongly_sized_read_is_asked_again) {
    for (const asked_again_read& read : ASKED_AGAIN_READS) {
        dpcd_firmware fw;
        fw.scripted_address = read.address;
        fw.scripted_requests = read.requests - 1;
        fw.scripted_reply = read.reply;
        fw.scripted_size = read.reply_size;

        dpcd_capabilities out = {};
        bool is_read = false;

        ASSERT_EQ(read_logged(fw, out, is_read), OK);

        for (const dpcd_firmware::call& call : fw.calls) {
            EXPECT_TRUE(call.is_zero_but_sent);
        }

        for (const expected_read& expected : EXTENDED_READS) {
            uint32_t expected_requests = expected.address == read.address ? read.requests : 1;
            EXPECT_EQ(fw.requests_at(expected.address), expected_requests);
        }

        EXPECT_EQ(fw.stray_count, 0u);
        EXPECT_TRUE(is_read);
        EXPECT_EQ(s_dpcd_log_bytes, 0u);

        EXPECT_TRUE(out.is_extended);
        EXPECT_EQ(string::memcmp(out.receiver, MONITOR_EXTENDED_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
        EXPECT_TRUE(out.has_phy_repeater_bytes);
        EXPECT_EQ(string::memcmp(out.phy_repeaters, PHY_REPEATER_CAPABILITIES, sizeof(PHY_REPEATER_CAPABILITIES)), 0);
    }
}

// Deferrals and acknowledgments of too many bytes are counted apart, so a byte at 0x0000E deferred 3 times and then
// acknowledged with 2 bytes 4 times is read at its 8th request
TEST(nvidia_display_dpcd, deferrals_and_acknowledgments_of_too_many_bytes_are_counted_apart) {
    dpcd_firmware fw;
    fw.scripted_address = NVIDIA_TRAINING_AUX_RD_INTERVAL;
    fw.scripted_requests = 3;
    fw.scripted_reply = NVIDIA_REPLY_DEFER;
    fw.later_requests = 4;
    fw.later_size = 2;

    dpcd_capabilities out = {};
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read), OK);

    EXPECT_EQ(fw.requests_at(NVIDIA_TRAINING_AUX_RD_INTERVAL), 8u);
    EXPECT_EQ(fw.requests_at(NVIDIA_EXTENDED_REV), 1u);
    EXPECT_EQ(fw.stray_count, 0u);
    EXPECT_TRUE(is_read);
    EXPECT_EQ(s_dpcd_log_bytes, 0u);
    EXPECT_TRUE(out.is_extended);
}

// A byte at 0x0000E acknowledged with 2 bytes by its first 7 requests ends its read at the 7th, though the monitor
// would answer an 8th with the byte, and has the field read from 0x00000
TEST(nvidia_display_dpcd, acknowledgments_of_too_many_bytes_end_a_read_at_its_count) {
    dpcd_firmware fw;
    fw.scripted_address = NVIDIA_TRAINING_AUX_RD_INTERVAL;
    fw.scripted_requests = NVIDIA_INTERVAL_REQUESTS;
    fw.scripted_size = 2;

    dpcd_capabilities out = {};
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read), OK);

    EXPECT_EQ(fw.requests_at(NVIDIA_TRAINING_AUX_RD_INTERVAL), NVIDIA_INTERVAL_REQUESTS);
    EXPECT_EQ(fw.requests_at(NVIDIA_DPCD_REV), 1u);
    EXPECT_EQ(fw.requests_at(NVIDIA_EXTENDED_REV), 0u);

    EXPECT_TRUE(is_read);
    EXPECT_FALSE(out.is_extended);

    EXPECT_TRUE(dpcd_log_contains("with a reply size of 2 bytes after 7 requests,"));
    EXPECT_TRUE(dpcd_log_contains(FIELD_FROM_BASE));
}

// Deferrals and acknowledgments of some but not all of the bytes share NVIDIA's count, so a field deferred 8 times and
// then acknowledged with 15 bytes 8 times ends a pass at its 16th request and is read at the 17th, in the next pass
TEST(nvidia_display_dpcd, deferrals_and_acknowledgments_of_some_of_the_bytes_share_a_count) {
    dpcd_firmware fw;
    fw.scripted_address = NVIDIA_EXTENDED_REV;
    fw.scripted_requests = 8;
    fw.scripted_reply = NVIDIA_REPLY_DEFER;
    fw.later_requests = 8;
    fw.later_size = 15;

    dpcd_capabilities out = {};
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read), OK);

    EXPECT_EQ(fw.requests_at(NVIDIA_TRAINING_AUX_RD_INTERVAL), 2u);
    EXPECT_EQ(fw.requests_at(NVIDIA_EXTENDED_REV), NVIDIA_CAPABILITY_REQUESTS + 1);

    EXPECT_TRUE(is_read);
    EXPECT_TRUE(out.is_extended);
    EXPECT_EQ(string::memcmp(out.receiver, MONITOR_EXTENDED_RECEIVER_CAPABILITIES, DATA_BYTES), 0);

    EXPECT_TRUE(dpcd_log_contains("with a reply size of 15 bytes after 16 requests,"));
    EXPECT_EQ(dpcd_log_count(READ_AGAIN), 1u);
}

static constexpr failing_answer INTERVAL_FAILURES[] = {
    {ERR_FIRMWARE, 0, 0, false, "display common refused display id 0x100's 1-byte AUX read at 0x0000E,"},
    {OK, NVIDIA_REPLY_NACK, 0, false, "a refusal from its monitor (reply type 1) after 1 request,"},
    {OK, NVIDIA_REPLY_DEFER, 0, true, "ask again later from its monitor (reply type 2) after 7 requests,"},
    {OK, NVIDIA_REPLY_TIMEOUT, 0, false, "no answer in time from its monitor (reply type 3) after 1 request,"},
    {OK, NVIDIA_REPLY_I2CNACK, 0, false, I2C_REFUSAL_LOGGED},
    {OK, NVIDIA_REPLY_I2CDEFER, 0, true, "ask again later from its monitor (reply type 8) after 7 requests,"},
    {OK, 0x10000, 0, false, "an unknown reply from its monitor (reply type 65536) after 1 request,"},
    {OK, 0x10000, 1, false, "an unknown reply from its monitor (reply type 65536) after 1 request,"},
    {OK, NVIDIA_REPLY_ACK, 0, false, "with a reply size of 0 bytes after 1 request,"},
    {OK, NVIDIA_REPLY_ACK, 2, true, "with a reply size of 2 bytes after 7 requests,"},
};

// A byte at 0x0000E that cannot be read, after 7 requests for an answer it is asked again on and one for any other,
// is logged and has the field read from 0x00000, then the PHY repeaters' bytes
TEST(nvidia_display_dpcd, the_field_is_read_from_0_when_the_byte_at_0x0000e_cannot_be_read) {
    for (const failing_answer& answer : INTERVAL_FAILURES) {
        dpcd_firmware fw;
        script_every_request(fw, NVIDIA_TRAINING_AUX_RD_INTERVAL, answer);
        dpcd_capabilities out = {};
        bool is_read = false;

        ASSERT_EQ(read_logged(fw, out, is_read), OK);

        uint32_t interval_requests = answer.is_asked_again ? NVIDIA_INTERVAL_REQUESTS : 1;
        EXPECT_EQ(fw.requests_at(NVIDIA_TRAINING_AUX_RD_INTERVAL), interval_requests);
        EXPECT_EQ(fw.requests_at(NVIDIA_DPCD_REV), 1u);
        EXPECT_EQ(fw.requests_at(NVIDIA_EXTENDED_REV), 0u);
        EXPECT_EQ(fw.requests_at(NVIDIA_LT_TUNABLE_PHY_REPEATER_REV), 1u);

        EXPECT_TRUE(is_read);
        EXPECT_FALSE(out.is_extended);
        EXPECT_EQ(string::memcmp(out.receiver, MONITOR_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
        EXPECT_TRUE(out.has_phy_repeater_bytes);

        EXPECT_TRUE(dpcd_log_contains("display id 0x100's "));
        EXPECT_TRUE(dpcd_log_contains(EXTENDED_READS[0].logged));
        EXPECT_TRUE(dpcd_log_contains(answer.logged));
        EXPECT_TRUE(dpcd_log_contains(FIELD_FROM_BASE));
    }
}

static constexpr failing_answer FIELD_FAILURES[] = {
    {ERR_FIRMWARE, 0, 0, false, "display common refused display id 0x100's 16-byte AUX read at 0x02200,"},
    {OK, NVIDIA_REPLY_NACK, 0, false, "a refusal from its monitor (reply type 1) after 1 request,"},
    {OK, NVIDIA_REPLY_DEFER, 0, true, "ask again later from its monitor (reply type 2) after 16 requests,"},
    {OK, NVIDIA_REPLY_TIMEOUT, 0, false, "no answer in time from its monitor (reply type 3) after 1 request,"},
    {OK, NVIDIA_REPLY_I2CDEFER, 0, true, "ask again later from its monitor (reply type 8) after 16 requests,"},
    {OK, 5, 0, false, "an unknown reply from its monitor (reply type 5) after 1 request,"},
    {OK, 0x10008, 0, false, "an unknown reply from its monitor (reply type 65544) after 1 request,"},
    {OK, 0x10000, 16, false, "an unknown reply from its monitor (reply type 65536) after 1 request,"},
    {OK, 0x10000, 15, false, "an unknown reply from its monitor (reply type 65536) after 1 request,"},
    {OK, NVIDIA_REPLY_ACK, 0, false, "with a reply size of 0 bytes after 1 request,"},
    {OK, NVIDIA_REPLY_ACK, 1, true, "with a reply size of 1 byte after 16 requests,"},
    {OK, NVIDIA_REPLY_ACK, 15, true, "with a reply size of 15 bytes after 16 requests,"},
    {OK, NVIDIA_REPLY_ACK, 17, true, "with a reply size of 17 bytes after 16 requests,"},
    {OK, NVIDIA_REPLY_ACK, 0x10010, true, "with a reply size of 65552 bytes after 16 requests,"},
};

// A field that cannot be read is logged and read again, the byte at 0x0000E before it, in each of NVIDIA's 3 more
// passes, leaving `is_read` cleared, `out` as it was and the PHY repeaters' bytes unread
TEST(nvidia_display_dpcd, a_field_that_cannot_be_read_is_read_again_in_3_more_passes) {
    for (const failing_answer& answer : FIELD_FAILURES) {
        dpcd_firmware fw;
        script_every_request(fw, NVIDIA_EXTENDED_REV, answer);
        dpcd_capabilities out;
        string::memset(&out, UNTOUCHED, sizeof(out));
        bool is_read = true;

        ASSERT_EQ(read_logged(fw, out, is_read), OK);

        uint32_t field_requests = answer.is_asked_again ? NVIDIA_CAPABILITY_REQUESTS : 1;
        EXPECT_EQ(fw.requests_at(NVIDIA_TRAINING_AUX_RD_INTERVAL), NVIDIA_PASSES);
        EXPECT_EQ(fw.requests_at(NVIDIA_DPCD_REV), 0u);
        EXPECT_EQ(fw.requests_at(NVIDIA_EXTENDED_REV), NVIDIA_PASSES * field_requests);
        EXPECT_EQ(fw.requests_at(NVIDIA_LT_TUNABLE_PHY_REPEATER_REV), 0u);

        EXPECT_FALSE(is_read);
        EXPECT_TRUE(capabilities_untouched(out));

        EXPECT_TRUE(dpcd_log_contains("display id 0x100's "));
        EXPECT_TRUE(dpcd_log_contains(EXTENDED_READS[1].logged));
        EXPECT_EQ(dpcd_log_count(answer.logged), NVIDIA_PASSES);
        EXPECT_EQ(dpcd_log_count(READ_AGAIN), NVIDIA_PASSES - 1);
        EXPECT_EQ(dpcd_log_count(NO_MORE_READ), 1u);
    }
}

// A field the monitor defers through a whole pass is read in the next, the byte at 0x0000E read again before it
TEST(nvidia_display_dpcd, a_field_read_in_a_later_pass_is_kept) {
    dpcd_firmware fw;
    fw.scripted_address = NVIDIA_EXTENDED_REV;
    fw.scripted_requests = NVIDIA_CAPABILITY_REQUESTS;
    fw.scripted_reply = NVIDIA_REPLY_DEFER;
    dpcd_capabilities out = {};
    bool is_read = false;

    ASSERT_EQ(read_logged(fw, out, is_read), OK);

    EXPECT_EQ(fw.requests_at(NVIDIA_TRAINING_AUX_RD_INTERVAL), 2u);
    EXPECT_EQ(fw.requests_at(NVIDIA_EXTENDED_REV), NVIDIA_CAPABILITY_REQUESTS + 1);
    EXPECT_EQ(fw.requests_at(NVIDIA_LT_TUNABLE_PHY_REPEATER_REV), 1u);

    EXPECT_TRUE(is_read);
    EXPECT_TRUE(out.is_extended);
    EXPECT_EQ(string::memcmp(out.receiver, MONITOR_EXTENDED_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
    EXPECT_TRUE(out.has_phy_repeater_bytes);
    EXPECT_EQ(dpcd_log_count(READ_AGAIN), 1u);
    EXPECT_EQ(dpcd_log_count(NO_MORE_READ), 0u);
}

static constexpr failing_answer REPEATER_FAILURES[] = {
    {ERR_FIRMWARE, 0, 0, false, "display common refused display id 0x100's 8-byte AUX read at 0xF0000,"},
    {OK, NVIDIA_REPLY_NACK, 0, false, "a refusal from its monitor (reply type 1) after 1 request,"},
    {OK, NVIDIA_REPLY_DEFER, 0, true, "ask again later from its monitor (reply type 2) after 16 requests,"},
    {OK, NVIDIA_REPLY_I2CNACK, 0, false, I2C_REFUSAL_LOGGED},
    {OK, 0x10000, 8, false, "an unknown reply from its monitor (reply type 65536) after 1 request,"},
    {OK, NVIDIA_REPLY_ACK, 0, false, "with a reply size of 0 bytes after 1 request,"},
    {OK, NVIDIA_REPLY_ACK, 7, true, "with a reply size of 7 bytes after 16 requests,"},
    {OK, NVIDIA_REPLY_ACK, 9, true, "with a reply size of 9 bytes after 16 requests,"},
};

// PHY repeaters' bytes that cannot be read are logged and taken as no PHY repeater, the monitor's capabilities read
// with `has_phy_repeater_bytes` cleared and the PHY repeaters' bytes 0
TEST(nvidia_display_dpcd, phy_repeaters_that_cannot_be_read_are_taken_as_none) {
    for (const failing_answer& answer : REPEATER_FAILURES) {
        dpcd_firmware fw;
        script_every_request(fw, NVIDIA_LT_TUNABLE_PHY_REPEATER_REV, answer);
        dpcd_capabilities out;
        string::memset(&out, UNTOUCHED, sizeof(out));
        bool is_read = false;

        ASSERT_EQ(read_logged(fw, out, is_read), OK);

        uint32_t repeater_requests = answer.is_asked_again ? NVIDIA_CAPABILITY_REQUESTS : 1;
        EXPECT_EQ(fw.requests_at(NVIDIA_TRAINING_AUX_RD_INTERVAL), 1u);
        EXPECT_EQ(fw.requests_at(NVIDIA_EXTENDED_REV), 1u);
        EXPECT_EQ(fw.requests_at(NVIDIA_LT_TUNABLE_PHY_REPEATER_REV), repeater_requests);

        EXPECT_TRUE(is_read);
        EXPECT_TRUE(out.is_extended);
        EXPECT_EQ(string::memcmp(out.receiver, MONITOR_EXTENDED_RECEIVER_CAPABILITIES, DATA_BYTES), 0);
        EXPECT_FALSE(out.has_phy_repeater_bytes);
        for (uint8_t byte : out.phy_repeaters) {
            EXPECT_EQ(byte, 0u);
        }

        EXPECT_TRUE(dpcd_log_contains("display id 0x100's "));
        EXPECT_TRUE(dpcd_log_contains(EXTENDED_READS[2].logged));
        EXPECT_TRUE(dpcd_log_contains(answer.logged));
        EXPECT_TRUE(dpcd_log_contains(NO_PHY_REPEATER));
    }
}

// Taking the DisplayPort hardware sends display common DP_SET_MANUAL_DISPLAYPORT for subdevice 0 in NVIDIA's layout,
// as NVIDIA's library sends it, and logs it
TEST(nvidia_display_dpcd, the_displayport_hardware_is_taken_with_nvidias_control) {
    dpcd_firmware fw;

    ASSERT_EQ(take_logged(fw), OK);

    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_EQ(fw.stray_count, 0u);
    EXPECT_EQ(fw.calls[0].object, TEST_DISPLAY_COMMON);
    EXPECT_EQ(fw.calls[0].command, NVIDIA_DP_SET_MANUAL_DISPLAYPORT);
    EXPECT_EQ(fw.calls[0].size, NVIDIA_DP_SET_MANUAL_DISPLAYPORT_BYTES);
    EXPECT_EQ(fw.manual_subdevice, 0u);

    EXPECT_TRUE(dpcd_log_contains("nvidia: display common leaves the DisplayPort hardware to the driver, "));
}

// A refusal is logged as the firmware keeping its own watermark programming, DP IRQ (CP IRQ) handling and retries of a
// deferred AUX reply, and gives `OK`, as NVIDIA's library goes on past it
TEST(nvidia_display_dpcd, a_refused_take_of_the_displayport_hardware_is_logged_and_goes_on) {
    dpcd_firmware fw;
    fw.manual_result = ERR_FIRMWARE;

    ASSERT_EQ(take_logged(fw), OK);

    EXPECT_EQ(fw.call_count, 1u);
    EXPECT_TRUE(dpcd_log_contains("nvidia: display common refused to leave the DisplayPort hardware to the driver, "));
    EXPECT_FALSE(dpcd_log_contains("leaves the DisplayPort hardware"));
}

// Any other error of the request is returned with nothing logged
TEST(nvidia_display_dpcd, other_errors_of_taking_the_displayport_hardware_are_returned) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_QUEUE_FULL, ERR_BAD_MESSAGE, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        dpcd_firmware fw;
        fw.manual_result = error;

        EXPECT_EQ(take_logged(fw), error);

        EXPECT_EQ(fw.call_count, 1u);
        EXPECT_EQ(s_dpcd_log_bytes, 0u);
    }
}

// The AUX control, its parameters' size, the most bytes a transaction carries, the read command, the reply types,
// DP_SET_MANUAL_DISPLAYPORT with its parameters' size, and the DPCD addresses, bit and lengths are NVIDIA's
TEST(nvidia_display_dpcd, displayport_constants_are_nvidias) {
    EXPECT_EQ(rm::CONTROL_DP_AUX, NVIDIA_DP_AUXCH_CTRL);
    EXPECT_EQ(sizeof(rm::aux_params), NVIDIA_DP_AUXCH_CTRL_BYTES);
    EXPECT_EQ(rm::AUX_DATA_MAX_BYTES, DATA_BYTES);
    EXPECT_EQ(rm::AUX_COMMAND_DPCD_READ, NVIDIA_NATIVE_READ);

    EXPECT_EQ(rm::AUX_REPLY_ACK, NVIDIA_REPLY_ACK);
    EXPECT_EQ(rm::AUX_REPLY_NACK, NVIDIA_REPLY_NACK);
    EXPECT_EQ(rm::AUX_REPLY_DEFER, NVIDIA_REPLY_DEFER);
    EXPECT_EQ(rm::AUX_REPLY_TIMEOUT, NVIDIA_REPLY_TIMEOUT);
    EXPECT_EQ(rm::AUX_REPLY_I2C_NACK, NVIDIA_REPLY_I2CNACK);
    EXPECT_EQ(rm::AUX_REPLY_I2C_DEFER, NVIDIA_REPLY_I2CDEFER);

    EXPECT_EQ(rm::CONTROL_SET_MANUAL_DISPLAYPORT, NVIDIA_DP_SET_MANUAL_DISPLAYPORT);
    EXPECT_EQ(sizeof(rm::manual_displayport_params), NVIDIA_DP_SET_MANUAL_DISPLAYPORT_BYTES);

    EXPECT_EQ(DPCD_TRAINING_AUX_READ_INTERVAL, NVIDIA_TRAINING_AUX_RD_INTERVAL);
    EXPECT_EQ(DPCD_RECEIVER_CAPABILITIES, NVIDIA_DPCD_REV);
    EXPECT_EQ(DPCD_EXTENDED_RECEIVER_CAPABILITIES, NVIDIA_EXTENDED_REV);
    EXPECT_EQ(DPCD_PHY_REPEATER_CAPABILITIES, NVIDIA_LT_TUNABLE_PHY_REPEATER_REV);

    EXPECT_EQ(DPCD_EXTENDED_COPY_PRESENT, NVIDIA_EXTENDED_RX_CAP);
    EXPECT_EQ(DPCD_RECEIVER_CAPABILITY_BYTES, NVIDIA_RECEIVER_CAPABILITY_BYTES);
    EXPECT_EQ(DPCD_PHY_REPEATER_CAPABILITY_BYTES, NVIDIA_PHY_REPEATER_CAPABILITY_BYTES);
}
