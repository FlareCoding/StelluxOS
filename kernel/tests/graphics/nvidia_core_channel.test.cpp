// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_core_channel);

using drivers::nvidia::allocate_core_channel;
using drivers::nvidia::allocate_window_channel;
using drivers::nvidia::chip_display;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::release_display_channel;
using drivers::nvidia::window_channel_area;
using drivers::nvidia::window_channel_number;
namespace rm = drivers::nvidia::rm;

// NVIDIA's commands, their parameters' sizes and fields' offsets, the values the driver sends, the core and window
// channels' areas in BAR0 and their numbers, restated to hold the requests to NVIDIA's layouts
static constexpr uint32_t NVIDIA_PUSH_BUFFER_CONTROL    = 0x20800A58;
static constexpr size_t   NVIDIA_PUSH_BUFFER_BYTES      = 40;
static constexpr uint32_t ADDRESS_SPACE_OFFSET          = 0;
static constexpr uint32_t ADDRESS_OFFSET                = 8;
static constexpr uint32_t LIMIT_OFFSET                  = 16;
static constexpr uint32_t SNOOP_OFFSET                  = 24;
static constexpr uint32_t CLASS_OFFSET                  = 28;
static constexpr uint32_t INSTANCE_OFFSET               = 32;
static constexpr uint32_t VALID_OFFSET                  = 36;
static constexpr uint32_t NVIDIA_SYSTEM_MEMORY          = 1;
static constexpr uint64_t NVIDIA_PUSH_BUFFER_LIMIT      = 0xFFF;
static constexpr size_t   NVIDIA_CHANNEL_PARAMS_BYTES   = 32;
static constexpr uint32_t CHANNEL_INSTANCE_OFFSET       = 0;
static constexpr uint32_t NVIDIA_RELEASE_FLAGS_CONTROL  = 0x50700117;
static constexpr size_t   NVIDIA_RELEASE_FLAGS_BYTES    = 8;
static constexpr uint32_t RELEASE_FLAGS_OFFSET          = 4;
static constexpr uint32_t NVIDIA_PRESERVE_HW            = 1;
static constexpr uint32_t NVIDIA_CORE_CHANNEL_AREA      = 0x680000;
static constexpr uint32_t NVIDIA_CORE_CHANNEL_NUMBER    = 0;
static constexpr uint32_t NVIDIA_WINDOW_CHANNEL_AREAS   = 0x690000;
static constexpr uint32_t NVIDIA_WINDOW_AREA_BYTES      = 4096;
static constexpr uint32_t NVIDIA_WINDOW_CHANNEL_NUMBERS = 1;

// The double's client, the display's handle and the first handle the double gives, a made-up push buffer address, a
// handle no call gives and a window other than window 0
static constexpr rm::handle TEST_CLIENT      = 0xC1D0000B;
static constexpr rm::handle TEST_DISPLAY     = 0x0005000B;
static constexpr rm::handle FIRST_HANDLE     = 0x0005000D;
static constexpr uint64_t   TEST_PUSH_BUFFER = 0x1234567000;
static constexpr rm::handle UNTOUCHED        = 0xDEAD0001;
static constexpr uint8_t    TEST_WINDOW      = 6;

// GA102's heads and windows with the display class left 0, as the channels' calls never use it, and core and window
// channel classes swapped from GA102's, so the test sees each class comes from its own field of the entry
static constexpr chip_display TEST_ENTRY = {
    .head_count                = 4,
    .window_count              = 8,
    .serial_link_count         = 4,
    .display_class             = 0,
    .core_channel_class        = 0xC67E,
    .window_channel_class      = 0xC67D,
    .heads_have_output_scalers = false,
    .displayport               = {},
};

// A firmware that records its allocations, controls, releases and internal controls in order and answers each with its
// own entry of `results`, and `OK` past them, filling a call's parameters with `reply_fill` on `OK`
class channel_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 4;
    static constexpr uint32_t MAX_BYTES = NVIDIA_PUSH_BUFFER_BYTES;

    enum class kind { allocation, control, release, internal_control };

    struct call {
        kind       made;
        rm::handle parent; // 0 but for an allocation
        rm::handle object; // 0 for an internal control
        uint32_t   number; // the class of an allocation or the command of a control, else 0
        size_t     size;
        uint8_t    sent[MAX_BYTES]; // the parameters' first bytes
    };

    call     calls[MAX_CALLS] = {};
    int32_t  results[MAX_CALLS] = {};
    uint8_t  reply_fill = 0;
    uint32_t call_count = 0;
    uint32_t handle_count = 0;

    int32_t allocate(rm::handle parent, rm::handle object, uint32_t class_id, void* params, size_t size) override {
        return answer(kind::allocation, parent, object, class_id, params, size);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        return answer(kind::control, 0, object, command, params, size);
    }

    int32_t release(rm::handle object) override {
        return answer(kind::release, 0, object, 0, nullptr, 0);
    }

    int32_t internal_control(uint32_t command, void* params, size_t size) override {
        return answer(kind::internal_control, 0, 0, command, params, size);
    }

    rm::handle new_handle() override {
        return FIRST_HANDLE + handle_count++;
    }

    rm::handle client() const override {
        return TEST_CLIENT;
    }

private:
    int32_t answer(kind made, rm::handle parent, rm::handle object, uint32_t number, void* params, size_t size) {
        call_count++;
        if (call_count > MAX_CALLS) {
            return OK;
        }

        uint32_t index = call_count - 1;
        calls[index] = {
            .made   = made,
            .parent = parent,
            .object = object,
            .number = number,
            .size   = size,
            .sent   = {},
        };

        size_t copied = size < MAX_BYTES ? size : MAX_BYTES;
        if (params != nullptr) {
            string::memcpy(calls[index].sent, params, copied);
        }

        if (results[index] == OK && params != nullptr) {
            string::memset(params, reply_fill, size);
        }

        return results[index];
    }
};

// The little-endian 32-bit value at `offset` of a call's parameters
static uint32_t word_at(const channel_firmware::call& made, uint32_t offset) {
    uint32_t value = 0;
    string::memcpy(&value, made.sent + offset, sizeof(value));

    return value;
}

// The little-endian 64-bit value at `offset` of a call's parameters
static uint64_t quad_at(const channel_firmware::call& made, uint32_t offset) {
    uint64_t value = 0;
    string::memcpy(&value, made.sent + offset, sizeof(value));

    return value;
}

// The core channel's area begins where NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_CORE places it
TEST(nvidia_core_channel, area_begins_at_nvidias_offset) {
    EXPECT_EQ(drivers::nvidia::CORE_CHANNEL_AREA, NVIDIA_CORE_CHANNEL_AREA);
}

// The core channel's number is NVIDIA's NV_PDISP_CHN_NUM_CORE
TEST(nvidia_core_channel, number_is_nvidias) {
    EXPECT_EQ(drivers::nvidia::CORE_CHANNEL_NUMBER, NVIDIA_CORE_CHANNEL_NUMBER);
}

// Each window's channel's area begins where NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_WIN places it, up to the last of
// NVIDIA's most windows
TEST(nvidia_core_channel, window_channel_areas_begin_at_nvidias_offsets) {
    EXPECT_EQ(window_channel_area(0), NVIDIA_WINDOW_CHANNEL_AREAS);
    EXPECT_EQ(window_channel_area(TEST_WINDOW), NVIDIA_WINDOW_CHANNEL_AREAS + TEST_WINDOW * NVIDIA_WINDOW_AREA_BYTES);
    EXPECT_EQ(window_channel_area(31), NVIDIA_WINDOW_CHANNEL_AREAS + 31 * NVIDIA_WINDOW_AREA_BYTES);
}

// Each window's channel's number is NVIDIA's NV_PDISP_CHN_NUM_WIN, the numbers following the core channel's, up to
// window 31, the last of the 32 windows NVIDIA numbers
TEST(nvidia_core_channel, window_channel_numbers_are_nvidias) {
    EXPECT_EQ(window_channel_number(0), NVIDIA_WINDOW_CHANNEL_NUMBERS);
    EXPECT_EQ(window_channel_number(TEST_WINDOW), NVIDIA_WINDOW_CHANNEL_NUMBERS + TEST_WINDOW);
    EXPECT_EQ(window_channel_number(31), NVIDIA_WINDOW_CHANNEL_NUMBERS + 31);
}

// A window channel's push buffer and allocation name the entry's window channel class and the window as the instance,
// as NVIDIA's modesetting module allocates each window's channel
TEST(nvidia_core_channel, window_channel_requests_name_its_class_and_window) {
    channel_firmware fw;
    rm::handle channel = UNTOUCHED;

    ASSERT_EQ(allocate_window_channel(fw, TEST_DISPLAY, TEST_ENTRY, TEST_WINDOW, TEST_PUSH_BUFFER, channel), OK);
    ASSERT_EQ(fw.call_count, 2u);

    const channel_firmware::call& location = fw.calls[0];
    EXPECT_TRUE(location.made == channel_firmware::kind::internal_control);
    EXPECT_EQ(location.number, NVIDIA_PUSH_BUFFER_CONTROL);
    EXPECT_EQ(quad_at(location, ADDRESS_OFFSET), TEST_PUSH_BUFFER);
    EXPECT_EQ(word_at(location, CLASS_OFFSET), TEST_ENTRY.window_channel_class);
    EXPECT_EQ(word_at(location, INSTANCE_OFFSET), uint32_t{TEST_WINDOW});

    const channel_firmware::call& allocation = fw.calls[1];
    EXPECT_TRUE(allocation.made == channel_firmware::kind::allocation);
    EXPECT_EQ(allocation.parent, TEST_DISPLAY);
    EXPECT_EQ(allocation.object, FIRST_HANDLE);
    EXPECT_EQ(allocation.number, TEST_ENTRY.window_channel_class);

    EXPECT_EQ(allocation.size, NVIDIA_CHANNEL_PARAMS_BYTES);
    EXPECT_EQ(word_at(allocation, CHANNEL_INSTANCE_OFFSET), uint32_t{TEST_WINDOW});

    EXPECT_EQ(channel, FIRST_HANDLE);
}

// The push buffer goes to the internal subdevice first in NVIDIA's layout, then the channel goes under the display with
// the next handle, the entry's class and its parameters zeroed at NVIDIA's size
TEST(nvidia_core_channel, push_buffer_goes_to_the_firmware_before_the_channel_in_nvidias_layout) {
    channel_firmware earlier;
    earlier.reply_fill = 0xFF;
    rm::handle earlier_channel = UNTOUCHED;
    channel_firmware fw;
    rm::handle channel = UNTOUCHED;

    // The earlier allocation's replies fill the stack the next allocation's requests are expected to reuse, so a
    // request not zeroed would likely carry them
    ASSERT_EQ(allocate_core_channel(earlier, TEST_DISPLAY, TEST_ENTRY, TEST_PUSH_BUFFER, earlier_channel), OK);
    ASSERT_EQ(allocate_core_channel(fw, TEST_DISPLAY, TEST_ENTRY, TEST_PUSH_BUFFER, channel), OK);

    ASSERT_EQ(fw.call_count, 2u);
    const channel_firmware::call& location = fw.calls[0];
    EXPECT_TRUE(location.made == channel_firmware::kind::internal_control);
    EXPECT_EQ(location.number, NVIDIA_PUSH_BUFFER_CONTROL);
    EXPECT_EQ(location.size, NVIDIA_PUSH_BUFFER_BYTES);

    EXPECT_EQ(word_at(location, ADDRESS_SPACE_OFFSET), NVIDIA_SYSTEM_MEMORY);
    EXPECT_EQ(quad_at(location, ADDRESS_OFFSET), TEST_PUSH_BUFFER);
    EXPECT_EQ(quad_at(location, LIMIT_OFFSET), NVIDIA_PUSH_BUFFER_LIMIT);
    EXPECT_EQ(word_at(location, SNOOP_OFFSET), 1u);

    EXPECT_EQ(word_at(location, CLASS_OFFSET), TEST_ENTRY.core_channel_class);
    EXPECT_EQ(word_at(location, INSTANCE_OFFSET), 0u);
    EXPECT_EQ(location.sent[VALID_OFFSET], 1u);

    const channel_firmware::call& allocation = fw.calls[1];
    EXPECT_TRUE(allocation.made == channel_firmware::kind::allocation);
    EXPECT_EQ(allocation.parent, TEST_DISPLAY);
    EXPECT_EQ(allocation.object, FIRST_HANDLE);
    EXPECT_EQ(allocation.number, TEST_ENTRY.core_channel_class);

    EXPECT_EQ(allocation.size, NVIDIA_CHANNEL_PARAMS_BYTES);
    for (uint32_t i = 0; i < NVIDIA_CHANNEL_PARAMS_BYTES; i++) {
        EXPECT_EQ(allocation.sent[i], 0u);
    }

    EXPECT_EQ(channel, FIRST_HANDLE);
}

// Every error for the push buffer ends the call before the allocation, and an allocation never sent or refused makes no
// further call, each returning its error with the handle unchanged
TEST(nvidia_core_channel, push_buffer_errors_and_unsent_or_refused_allocations_leave_the_handle_unchanged) {
    const int32_t push_buffer_errors[] = {
        ERR_TOO_LARGE, ERR_NO_MEMORY, ERR_QUEUE_FULL, ERR_TIMEOUT, ERR_FIRMWARE, ERR_BAD_MESSAGE,
    };

    const int32_t allocation_errors[] = {ERR_TOO_LARGE, ERR_NO_MEMORY, ERR_QUEUE_FULL, ERR_FIRMWARE};
    rm::handle channel = UNTOUCHED;

    for (int32_t error : push_buffer_errors) {
        channel_firmware fw;
        fw.results[0] = error;

        EXPECT_EQ(allocate_core_channel(fw, TEST_DISPLAY, TEST_ENTRY, TEST_PUSH_BUFFER, channel), error);
        EXPECT_EQ(fw.call_count, 1u);
    }

    for (int32_t error : allocation_errors) {
        channel_firmware fw;
        fw.results[1] = error;

        EXPECT_EQ(allocate_core_channel(fw, TEST_DISPLAY, TEST_ENTRY, TEST_PUSH_BUFFER, channel), error);
        EXPECT_EQ(fw.call_count, 2u);
    }

    EXPECT_EQ(channel, UNTOUCHED);
}

// An allocation failing with `ERR_TIMEOUT` or `ERR_BAD_MESSAGE` may leave the firmware holding the channel, so its
// handle is given out with the error, for the channel's release
TEST(nvidia_core_channel, allocations_the_firmware_may_hold_give_out_the_handle) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_BAD_MESSAGE};
    for (int32_t error : errors) {
        channel_firmware fw;
        fw.results[1] = error;
        rm::handle channel = UNTOUCHED;

        EXPECT_EQ(allocate_core_channel(fw, TEST_DISPLAY, TEST_ENTRY, TEST_PUSH_BUFFER, channel), error);
        EXPECT_EQ(fw.call_count, 2u);
        EXPECT_EQ(channel, FIRST_HANDLE);
    }
}

// The display is asked to keep the hardware as it is, with the subdevice's index 0 and NVIDIA's PRESERVE_HW, then the
// channel is released
TEST(nvidia_core_channel, release_asks_to_keep_the_hardware_then_releases_the_channel) {
    channel_firmware earlier;
    earlier.reply_fill = 0xFF;
    channel_firmware fw;

    // The earlier release's reply fills the stack the next release's request is expected to reuse, so a request not
    // zeroed would likely carry it
    ASSERT_EQ(release_display_channel(earlier, TEST_DISPLAY, FIRST_HANDLE, true), OK);
    EXPECT_EQ(release_display_channel(fw, TEST_DISPLAY, FIRST_HANDLE, true), OK);

    ASSERT_EQ(fw.call_count, 2u);
    const channel_firmware::call& flags = fw.calls[0];
    EXPECT_TRUE(flags.made == channel_firmware::kind::control);
    EXPECT_EQ(flags.object, TEST_DISPLAY);
    EXPECT_EQ(flags.number, NVIDIA_RELEASE_FLAGS_CONTROL);

    EXPECT_EQ(flags.size, NVIDIA_RELEASE_FLAGS_BYTES);
    EXPECT_EQ(word_at(flags, 0), 0u);
    EXPECT_EQ(word_at(flags, RELEASE_FLAGS_OFFSET), NVIDIA_PRESERVE_HW);

    const channel_firmware::call& release = fw.calls[1];
    EXPECT_TRUE(release.made == channel_firmware::kind::release);
    EXPECT_EQ(release.object, FIRST_HANDLE);
}

// A release keeping nothing asks for no flag, as the engine's does once an update changed the boot firmware's state
TEST(nvidia_core_channel, release_without_keeping_the_hardware_asks_for_no_flag) {
    channel_firmware fw;

    EXPECT_EQ(release_display_channel(fw, TEST_DISPLAY, FIRST_HANDLE, false), OK);

    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_TRUE(fw.calls[0].made == channel_firmware::kind::release);
    EXPECT_EQ(fw.calls[0].object, FIRST_HANDLE);
}

// The channel is released even after a refused flag, and the first failure's error is the one returned
TEST(nvidia_core_channel, release_follows_a_refused_flag_and_returns_the_first_error) {
    channel_firmware refusing_flag;
    refusing_flag.results[0] = ERR_FIRMWARE;

    channel_firmware failing_release;
    failing_release.results[1] = ERR_TIMEOUT;

    channel_firmware failing_both;
    failing_both.results[0] = ERR_TIMEOUT;
    failing_both.results[1] = ERR_FIRMWARE;

    EXPECT_EQ(release_display_channel(refusing_flag, TEST_DISPLAY, FIRST_HANDLE, true), ERR_FIRMWARE);
    EXPECT_EQ(release_display_channel(failing_release, TEST_DISPLAY, FIRST_HANDLE, true), ERR_TIMEOUT);
    EXPECT_EQ(release_display_channel(failing_both, TEST_DISPLAY, FIRST_HANDLE, true), ERR_TIMEOUT);

    const channel_firmware* released[] = {&refusing_flag, &failing_release, &failing_both};
    for (const channel_firmware* fw : released) {
        ASSERT_EQ(fw->call_count, 2u);
        EXPECT_TRUE(fw->calls[1].made == channel_firmware::kind::release);
        EXPECT_EQ(fw->calls[1].object, FIRST_HANDLE);
    }
}
