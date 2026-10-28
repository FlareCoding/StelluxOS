// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_core_channel_idle);

using drivers::nvidia::chip_display;
using drivers::nvidia::CORE_CHANNEL_IDLE_WAIT_NS;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::NS_PER_MS;
using drivers::nvidia::NS_PER_US;
using drivers::nvidia::OK;
using drivers::nvidia::wait_core_channel_idle;
namespace rm = drivers::nvidia::rm;

// NVIDIA's GET_CHANNEL_INFO, its parameters' size, the offsets of the fields sent in them or read from them and the
// idle, busy and unconnected states, restated so the tests hold the requests to NVIDIA's layout
static constexpr uint32_t NVIDIA_GET_CHANNEL_INFO       = 0xC3700104;
static constexpr size_t   NVIDIA_GET_CHANNEL_INFO_BYTES = 20;
static constexpr uint32_t SUBDEVICE_OFFSET              = 0;
static constexpr uint32_t CHANNEL_CLASS_OFFSET          = 4;
static constexpr uint32_t CHANNEL_INSTANCE_OFFSET       = 8;
static constexpr uint32_t DEBUG_MODE_OFFSET             = 12;
static constexpr uint32_t CHANNEL_STATE_OFFSET          = 16;
static constexpr uint32_t NVIDIA_STATE_IDLE             = 0x1;
static constexpr uint32_t NVIDIA_STATE_BUSY             = 0x40;
static constexpr uint32_t NVIDIA_STATE_UNCONNECTED      = 0x20000;

// The state's highest bit, which names no state of NVIDIA's
static constexpr uint32_t UNNAMED_STATE_BIT = 0x80000000;

// The display's handle and the core channel's class, made up, the class past 16 bits so a request shows it comes
// whole from the chip entry given
static constexpr rm::handle TEST_DISPLAY            = 0x0005000C;
static constexpr uint32_t   TEST_CORE_CHANNEL_CLASS = 0x1C6FD;

// NVIDIA's wait for the core channel's idle state, 100000 microseconds, restated
static constexpr uint64_t NVIDIA_IDLE_TIMEOUT_US = 100000;

// How long after its first request the double answers every request with `ERR_TIMEOUT`, so a wait still running a
// second after its first request fails instead of hanging the suite
static constexpr uint64_t LATE_REQUEST_NS = 1000 * NS_PER_MS;

// A firmware that answers GET_CHANNEL_INFO with `busy_replies` busy replies, then idle ones, each reply's other bytes
// all ones, failing request number `failing_request` with `failed_result` and every late request with `ERR_TIMEOUT`
class channel_state_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS   = 8;
    static constexpr uint32_t ALWAYS_BUSY = 0xFFFFFFFF;
    static constexpr uint32_t NO_FAILURE  = 0xFFFFFFFF;

    struct call {
        rm::handle object;
        uint32_t   command;
        size_t     size;
        uint8_t    sent[NVIDIA_GET_CHANNEL_INFO_BYTES];
    };

    call     calls[MAX_CALLS] = {};
    uint32_t call_count = 0;
    uint32_t stray_count = 0;
    uint64_t first_request_ns = 0;

    uint32_t busy_replies = 0;               // replies before the first idle one, or `ALWAYS_BUSY` for none idle
    uint32_t busy_state = NVIDIA_STATE_BUSY; // the state a busy reply names
    int32_t  busy_result = OK;               // `ERR_FIRMWARE` to refuse those requests, still writing `busy_state`
    uint32_t failing_request = NO_FAILURE;
    int32_t  failed_result = OK;

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        uint32_t request = call_count;
        record(object, command, params, size);

        bool is_state_request = command == NVIDIA_GET_CHANNEL_INFO && size == NVIDIA_GET_CHANNEL_INFO_BYTES;
        if (params == nullptr || !is_state_request) {
            stray_count++;
            return OK;
        }

        bool is_late = clock::now_ns() - first_request_ns > LATE_REQUEST_NS;
        if (is_late) {
            return ERR_TIMEOUT;
        }

        if (request == failing_request) {
            return failed_result;
        }

        bool is_busy = request < busy_replies;
        uint32_t state = is_busy ? busy_state : NVIDIA_STATE_IDLE;

        string::memset(params, 0xFF, size);
        string::memcpy(static_cast<uint8_t*>(params) + CHANNEL_STATE_OFFSET, &state, sizeof(state));

        return is_busy ? busy_result : OK;
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
    // Counts the call, notes when the first came, and for one of the first `MAX_CALLS` keeps its object, command, size
    // and first 20 bytes sent
    void record(rm::handle object, uint32_t command, void* params, size_t size) {
        call_count++;
        if (call_count == 1) {
            first_request_ns = clock::now_ns();
        }

        if (call_count > MAX_CALLS) {
            return;
        }

        call& recorded = calls[call_count - 1];
        recorded.object = object;
        recorded.command = command;
        recorded.size = size;
        if (params != nullptr) {
            size_t kept = size < NVIDIA_GET_CHANNEL_INFO_BYTES ? size : NVIDIA_GET_CHANNEL_INFO_BYTES;
            string::memcpy(recorded.sent, params, kept);
        }
    }
};

// Waits for the core channel of a chip entry naming `TEST_CORE_CHANNEL_CLASS`
static int32_t wait_idle(channel_state_firmware& fw, bool& is_idle) {
    chip_display chip = {};
    chip.core_channel_class = TEST_CORE_CHANNEL_CLASS;

    return wait_core_channel_idle(fw, TEST_DISPLAY, chip, is_idle);
}

// The 32-bit field at `offset` of a recorded request
static uint32_t sent_field(const channel_state_firmware::call& sent, uint32_t offset) {
    uint32_t value = 0;
    string::memcpy(&value, sent.sent + offset, sizeof(value));
    return value;
}

// Each request goes to the display in NVIDIA's layout, zeroed but for the core channel's class, so it names instance 0
// on the first GPU and carries none of the bytes an earlier reply wrote, the wait's own busy reply included
TEST(nvidia_core_channel_idle, requests_go_to_the_display_in_nvidias_layout) {
    channel_state_firmware earlier;
    earlier.busy_replies = 1;
    bool was_idle = false;

    channel_state_firmware fw;
    fw.busy_replies = 1;
    bool is_idle = false;

    // The earlier wait's replies of all ones fill the stack the next wait's requests are expected to reuse
    ASSERT_EQ(wait_idle(earlier, was_idle), OK);
    ASSERT_EQ(wait_idle(fw, is_idle), OK);

    ASSERT_EQ(fw.call_count, 2u);
    EXPECT_EQ(fw.stray_count, 0u);
    for (uint32_t i = 0; i < 2; i++) {
        const channel_state_firmware::call& sent = fw.calls[i];
        EXPECT_EQ(sent.object, TEST_DISPLAY);
        EXPECT_EQ(sent.command, NVIDIA_GET_CHANNEL_INFO);
        EXPECT_EQ(sent.size, NVIDIA_GET_CHANNEL_INFO_BYTES);

        EXPECT_EQ(sent_field(sent, SUBDEVICE_OFFSET), 0u);
        EXPECT_EQ(sent_field(sent, CHANNEL_CLASS_OFFSET), TEST_CORE_CHANNEL_CLASS);
        EXPECT_EQ(sent_field(sent, CHANNEL_INSTANCE_OFFSET), 0u);
        EXPECT_EQ(sent.sent[DEBUG_MODE_OFFSET], 0u);
        EXPECT_EQ(sent_field(sent, CHANNEL_STATE_OFFSET), 0u);
    }
}

// The wait for the idle state is NVIDIA's
TEST(nvidia_core_channel_idle, wait_is_nvidias) {
    EXPECT_EQ(CORE_CHANNEL_IDLE_WAIT_NS, NVIDIA_IDLE_TIMEOUT_US * NS_PER_US);
}

// A first reply naming the idle state ends the wait with no second request
TEST(nvidia_core_channel_idle, an_idle_first_reply_ends_the_wait) {
    channel_state_firmware fw;
    bool is_idle = false;

    ASSERT_EQ(wait_idle(fw, is_idle), OK);
    EXPECT_EQ(fw.call_count, 1u);
    EXPECT_EQ(fw.stray_count, 0u);
    EXPECT_TRUE(is_idle);
}

// Each reply not naming the idle state alone, even one naming no state or the idle state with another bit, and a
// refused request, even with the idle state in its parameters, count as busy, so the display is asked again until idle
TEST(nvidia_core_channel_idle, requests_repeat_after_busy_replies_and_refusals) {
    const uint32_t busy_states[] = {
        NVIDIA_STATE_BUSY,
        NVIDIA_STATE_IDLE | NVIDIA_STATE_BUSY,
        0,
        NVIDIA_STATE_IDLE | NVIDIA_STATE_UNCONNECTED,
        NVIDIA_STATE_IDLE | UNNAMED_STATE_BIT,
        NVIDIA_STATE_IDLE,
    };

    const int32_t busy_results[] = {OK, OK, OK, OK, OK, ERR_FIRMWARE};
    for (uint32_t i = 0; i < 6; i++) {
        channel_state_firmware fw;
        fw.busy_replies = 3;
        fw.busy_state = busy_states[i];
        fw.busy_result = busy_results[i];
        bool is_idle = false;

        ASSERT_EQ(wait_idle(fw, is_idle), OK);
        EXPECT_EQ(fw.call_count, 4u);
        EXPECT_EQ(fw.stray_count, 0u);
        EXPECT_TRUE(is_idle);
    }
}

// With no reply reporting the channel idle, whether each reply names the busy state or the firmware refuses each
// request, the wait gives up after more than 100 milliseconds, asking more often than pauses of 100 microseconds allow
TEST(nvidia_core_channel_idle, wait_gives_up_after_100_milliseconds_without_idle) {
    const int32_t busy_results[] = {OK, ERR_FIRMWARE};
    for (int32_t busy_result : busy_results) {
        channel_state_firmware fw;
        fw.busy_replies = channel_state_firmware::ALWAYS_BUSY;
        fw.busy_result = busy_result;
        bool is_idle = true;

        uint64_t started_ns = clock::now_ns();
        ASSERT_EQ(wait_idle(fw, is_idle), OK);
        uint64_t waited_ns = clock::now_ns() - started_ns;

        // A pause of 100 microseconds between requests would allow at most 1002 of them before the wait gives up
        EXPECT_GT(waited_ns, NVIDIA_IDLE_TIMEOUT_US * NS_PER_US);
        EXPECT_GT(fw.call_count, 1002u);
        EXPECT_EQ(fw.stray_count, 0u);
        EXPECT_FALSE(is_idle);
    }
}

// Any error but `ERR_FIRMWARE`, of the first request or of one after a busy reply, ends the wait with it, sending no
// later request and leaving `is_idle` unchanged
TEST(nvidia_core_channel_idle, other_errors_end_the_wait) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_BAD_MESSAGE, ERR_QUEUE_FULL, ERR_NO_MEMORY, ERR_TOO_LARGE};
    for (int32_t error : errors) {
        for (uint32_t failing_request = 0; failing_request < 2; failing_request++) {
            channel_state_firmware fw;
            fw.busy_replies = channel_state_firmware::ALWAYS_BUSY;
            fw.failing_request = failing_request;
            fw.failed_result = error;
            bool is_idle = true;

            EXPECT_EQ(wait_idle(fw, is_idle), error);
            EXPECT_EQ(fw.call_count, failing_request + 1);
            EXPECT_TRUE(is_idle);
        }
    }
}
