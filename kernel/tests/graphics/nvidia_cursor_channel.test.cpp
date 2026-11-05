// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/cursor_channel.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"

TEST_SUITE(nvidia_cursor_channel);

using drivers::nvidia::chip_display;
using drivers::nvidia::cursor_channel;
using drivers::nvidia::cursor_control_page;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::registers;
namespace rm = drivers::nvidia::rm;

// NVIDIA's push buffer control and its fields' offsets, the cursor channel's allocation parameters' size and fields'
// offsets, the control pages' place in BAR0 and their registers, restated to hold the driver to NVIDIA's layouts
static constexpr uint32_t NVIDIA_PUSH_BUFFER_CONTROL = 0x20800A58;
static constexpr size_t   NVIDIA_PUSH_BUFFER_BYTES   = 40;
static constexpr uint32_t ADDRESS_SPACE_OFFSET       = 0;
static constexpr uint32_t ADDRESS_OFFSET             = 8;
static constexpr uint32_t LIMIT_OFFSET               = 16;
static constexpr uint32_t SNOOP_OFFSET               = 24;
static constexpr uint32_t CLASS_OFFSET               = 28;
static constexpr uint32_t INSTANCE_OFFSET            = 32;
static constexpr uint32_t VALID_OFFSET               = 36;
static constexpr size_t   NVIDIA_PIO_PARAMS_BYTES    = 16;
static constexpr uint32_t PIO_INSTANCE_OFFSET        = 0;
static constexpr uint32_t PIO_NOTIFY_OFFSET          = 4;
static constexpr uint32_t PIO_CONTROL_OFFSET         = 8;
static constexpr uint32_t NVIDIA_CURSOR_PAGES        = 0x6D8000;
static constexpr uint32_t NVIDIA_CURSOR_PAGE_BYTES   = 4096;
static constexpr uint32_t NVIDIA_FREE                = 0x8;
static constexpr uint32_t NVIDIA_UPDATE              = 0x200;
static constexpr uint32_t NVIDIA_HOT_SPOT_POINT      = 0x208;

// NVIDIA's NV_PDISP_CHN_NUM_CURS__SIZE_1, the count of cursor channel numbers, one for each of heads 0 to 7
static constexpr uint8_t NVIDIA_CURSOR_CHANNELS = 8;

// The double's client, the display's handle and the first handle the double gives, a head other than head 0, the
// heads a GPU of 4 reports present and a cursor class unlike GA102's, so the test sees it comes from the entry
static constexpr rm::handle TEST_CLIENT        = 0xC1D0000B;
static constexpr rm::handle TEST_DISPLAY       = 0x0005000B;
static constexpr rm::handle FIRST_HANDLE       = 0x0005000D;
static constexpr uint8_t    TEST_HEAD          = 2;
static constexpr uint32_t   FOUR_HEADS         = 0xF;
static constexpr uint32_t   TEST_CURSOR_CLASS  = 0xC57A;
static constexpr uint32_t   UNTOUCHED_REGISTER = 0xDEADBEEF;

// GA102's heads and windows with every class but the cursor channel's left 0, as the cursor channel never uses them
static constexpr chip_display TEST_ENTRY = {
    .head_count                = 4,
    .window_count              = 8,
    .serial_link_count         = 4,
    .display_class             = 0,
    .core_channel_class        = 0,
    .window_channel_class      = 0,
    .heads_have_output_scalers = false,
    .cursor_channel_class      = TEST_CURSOR_CLASS,
    .displayport               = {},
};

// A firmware that records its allocations, controls, releases and internal controls in order and answers each with its
// own entry of `results`, and `OK` past them
class cursor_firmware : public firmware {
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

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

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

        return results[index];
    }
};

// A value the free register reads, what a write then returns and whether the method reaches the control page
struct free_case {
    uint32_t free;
    int32_t  rc;
    bool     writes;
};

// The little-endian 32-bit value at `offset` of a call's parameters
static uint32_t word_at(const cursor_firmware::call& made, uint32_t offset) {
    uint32_t value = 0;
    string::memcpy(&value, made.sent + offset, sizeof(value));

    return value;
}

// The little-endian 64-bit value at `offset` of a call's parameters
static uint64_t quad_at(const cursor_firmware::call& made, uint32_t offset) {
    uint64_t value = 0;
    string::memcpy(&value, made.sent + offset, sizeof(value));

    return value;
}

// The words standing in for the test head's control page, and a view of BAR0 that reaches them there
static uint32_t g_control_page[NVIDIA_CURSOR_PAGE_BYTES / 4];

static registers test_bar0() {
    return registers(reinterpret_cast<uintptr_t>(g_control_page) - cursor_control_page(TEST_HEAD));
}

// Each head's control page begins where NVIDIA's NV_UDISP_FE_CHN_ASSY_BASEADR_CURS places it, up to head 7
TEST(nvidia_cursor_channel, control_pages_begin_at_nvidias_offsets) {
    EXPECT_EQ(cursor_control_page(0), NVIDIA_CURSOR_PAGES);
    EXPECT_EQ(cursor_control_page(TEST_HEAD), NVIDIA_CURSOR_PAGES + TEST_HEAD * NVIDIA_CURSOR_PAGE_BYTES);
    EXPECT_EQ(cursor_control_page(7), NVIDIA_CURSOR_PAGES + 7 * NVIDIA_CURSOR_PAGE_BYTES);
}

// The free register, `UPDATE` and the position are NVIDIA's NVC67A registers
TEST(nvidia_cursor_channel, registers_are_nvidias) {
    EXPECT_EQ(cursor_channel::FREE, NVIDIA_FREE);
    EXPECT_EQ(cursor_channel::UPDATE, NVIDIA_UPDATE);
    EXPECT_EQ(cursor_channel::HOT_SPOT_POINT, NVIDIA_HOT_SPOT_POINT);
}

// The push buffer control goes first with no push buffer, naming only the entry's cursor class and the head, then the
// allocation under the display with the head as the instance and no notifier, as NVIDIA's modesetting module asks
TEST(nvidia_cursor_channel, allocation_follows_a_push_buffer_control_without_a_push_buffer) {
    cursor_firmware fw;
    registers bar0 = test_bar0();
    cursor_channel channel;

    int32_t rc = channel.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, TEST_HEAD, FOUR_HEADS);

    EXPECT_EQ(rc, OK);
    ASSERT_EQ(fw.call_count, 2u);

    const cursor_firmware::call& location = fw.calls[0];
    EXPECT_TRUE(location.made == cursor_firmware::kind::internal_control);
    EXPECT_EQ(location.number, NVIDIA_PUSH_BUFFER_CONTROL);
    EXPECT_EQ(location.size, NVIDIA_PUSH_BUFFER_BYTES);

    EXPECT_EQ(word_at(location, ADDRESS_SPACE_OFFSET), 0u);
    EXPECT_EQ(quad_at(location, ADDRESS_OFFSET), 0u);
    EXPECT_EQ(quad_at(location, LIMIT_OFFSET), 0u);
    EXPECT_EQ(word_at(location, SNOOP_OFFSET), 0u);

    EXPECT_EQ(word_at(location, CLASS_OFFSET), TEST_CURSOR_CLASS);
    EXPECT_EQ(word_at(location, INSTANCE_OFFSET), static_cast<uint32_t>(TEST_HEAD));
    EXPECT_EQ(location.sent[VALID_OFFSET], 0u);

    const cursor_firmware::call& allocation = fw.calls[1];
    EXPECT_TRUE(allocation.made == cursor_firmware::kind::allocation);
    EXPECT_EQ(allocation.parent, TEST_DISPLAY);
    EXPECT_EQ(allocation.object, FIRST_HANDLE);
    EXPECT_EQ(allocation.number, TEST_CURSOR_CLASS);
    EXPECT_EQ(allocation.size, NVIDIA_PIO_PARAMS_BYTES);

    EXPECT_EQ(word_at(allocation, PIO_INSTANCE_OFFSET), static_cast<uint32_t>(TEST_HEAD));
    EXPECT_EQ(word_at(allocation, PIO_NOTIFY_OFFSET), 0u);
    EXPECT_EQ(quad_at(allocation, PIO_CONTROL_OFFSET), 0u);
}

// A head the present heads lack, or one past the 8 NVIDIA numbers cursor channels for, is refused before any request
// and leaves nothing to release
TEST(nvidia_cursor_channel, a_head_not_present_is_refused_before_any_request) {
    cursor_firmware fw;
    registers bar0 = test_bar0();
    cursor_channel missing;
    cursor_channel past_the_last;

    uint32_t others = FOUR_HEADS & ~(1u << TEST_HEAD);
    int32_t missing_rc = missing.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, TEST_HEAD, others);
    int32_t past_rc = past_the_last.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, NVIDIA_CURSOR_CHANNELS, 0xFFFFFFFF);
    int32_t missing_release_rc = missing.release(fw);
    int32_t past_release_rc = past_the_last.release(fw);

    EXPECT_EQ(missing_rc, ERR_INVALID_ARG);
    EXPECT_EQ(past_rc, ERR_INVALID_ARG);
    EXPECT_EQ(missing_release_rc, OK);
    EXPECT_EQ(past_release_rc, OK);
    EXPECT_EQ(fw.call_count, 0u);
}

// The last of the 8 heads NVIDIA numbers cursor channels for, with every head present, is allocated as its instance
TEST(nvidia_cursor_channel, the_last_numbered_head_is_allocated_as_its_instance) {
    cursor_firmware fw;
    registers bar0 = test_bar0();
    cursor_channel channel;

    uint8_t last_head = NVIDIA_CURSOR_CHANNELS - 1;
    int32_t rc = channel.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, last_head, 0xFFFFFFFF);

    EXPECT_EQ(rc, OK);
    ASSERT_EQ(fw.call_count, 2u);
    EXPECT_EQ(word_at(fw.calls[0], INSTANCE_OFFSET), static_cast<uint32_t>(last_head));
    EXPECT_EQ(word_at(fw.calls[1], PIO_INSTANCE_OFFSET), static_cast<uint32_t>(last_head));
}

// Every error of the push buffer control ends the call before the allocation and leaves nothing to release
TEST(nvidia_cursor_channel, every_push_buffer_control_error_ends_the_allocation) {
    const int32_t errors[] = {ERR_TOO_LARGE, ERR_NO_MEMORY, ERR_QUEUE_FULL, ERR_TIMEOUT, ERR_FIRMWARE, ERR_BAD_MESSAGE};
    for (int32_t error : errors) {
        cursor_firmware fw;
        fw.results[0] = error;
        registers bar0 = test_bar0();
        cursor_channel channel;

        int32_t rc = channel.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, TEST_HEAD, FOUR_HEADS);
        int32_t release_rc = channel.release(fw);

        EXPECT_EQ(rc, error);
        EXPECT_EQ(release_rc, OK);
        EXPECT_EQ(fw.call_count, 1u);
    }
}

// An allocation refused or never sent leaves nothing to release, while one the firmware may hold, its reply late or
// garbled, is released, the release returning its own request's result
TEST(nvidia_cursor_channel, an_allocation_the_firmware_may_hold_is_released) {
    const int32_t dropped_errors[] = {ERR_TOO_LARGE, ERR_NO_MEMORY, ERR_QUEUE_FULL, ERR_FIRMWARE};
    const int32_t kept_errors[] = {ERR_TIMEOUT, ERR_BAD_MESSAGE};
    registers bar0 = test_bar0();

    for (int32_t error : dropped_errors) {
        cursor_firmware fw;
        fw.results[1] = error;
        cursor_channel dropped;

        int32_t rc = dropped.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, TEST_HEAD, FOUR_HEADS);
        int32_t release_rc = dropped.release(fw);

        EXPECT_EQ(rc, error);
        EXPECT_EQ(release_rc, OK);
        EXPECT_EQ(fw.call_count, 2u);
    }

    for (int32_t error : kept_errors) {
        cursor_firmware fw;
        fw.results[1] = error;
        fw.results[2] = ERR_FIRMWARE;
        cursor_channel kept;

        int32_t rc = kept.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, TEST_HEAD, FOUR_HEADS);
        int32_t release_rc = kept.release(fw);

        EXPECT_EQ(rc, error);
        EXPECT_EQ(release_rc, ERR_FIRMWARE);
        ASSERT_EQ(fw.call_count, 3u);
        EXPECT_TRUE(fw.calls[2].made == cursor_firmware::kind::release);
        EXPECT_EQ(fw.calls[2].object, FIRST_HANDLE);
    }
}

// The release frees the channel's object alone, with no control asking for the hardware kept, and only once
TEST(nvidia_cursor_channel, release_frees_the_channel_once_without_keeping_the_hardware) {
    cursor_firmware fw;
    registers bar0 = test_bar0();
    cursor_channel channel;
    int32_t rc = channel.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, TEST_HEAD, FOUR_HEADS);

    int32_t first_rc = channel.release(fw);
    int32_t second_rc = channel.release(fw);

    EXPECT_EQ(rc, OK);
    EXPECT_EQ(first_rc, OK);
    EXPECT_EQ(second_rc, OK);

    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_TRUE(fw.calls[2].made == cursor_firmware::kind::release);
    EXPECT_EQ(fw.calls[2].object, FIRST_HANDLE);
}

// A write takes the method into the head's control page for any free register value but 0 and all ones, even with
// the count in bits 5:0 at 0. The test of the whole register is NVIDIA's, and refusing 0 and all ones this driver's
static constexpr free_case FREE_CASES[] = {
    {.free = 0, .rc = ERR_QUEUE_FULL, .writes = false},
    {.free = 1, .rc = OK, .writes = true},
    {.free = 0x3F, .rc = OK, .writes = true},
    {.free = 0x40, .rc = OK, .writes = true},
    {.free = 0x7FFFFFFF, .rc = OK, .writes = true},
    {.free = 0x80000000, .rc = OK, .writes = true},
    {.free = 0xFFFFFFFE, .rc = OK, .writes = true},
    {.free = 0xFFFFFFFF, .rc = ERR_BAD_STATE, .writes = false},
};

TEST(nvidia_cursor_channel, a_write_takes_room_from_the_whole_free_register) {
    cursor_firmware fw;
    registers bar0 = test_bar0();
    cursor_channel channel;
    ASSERT_EQ(channel.allocate(fw, bar0, TEST_DISPLAY, TEST_ENTRY, TEST_HEAD, FOUR_HEADS), OK);

    for (const free_case& tried : FREE_CASES) {
        g_control_page[NVIDIA_FREE / 4] = tried.free;
        g_control_page[NVIDIA_HOT_SPOT_POINT / 4] = UNTOUCHED_REGISTER;

        int32_t rc = channel.write(NVIDIA_HOT_SPOT_POINT, 0x00400020);

        uint32_t expected = tried.writes ? 0x00400020u : UNTOUCHED_REGISTER;
        EXPECT_EQ(rc, tried.rc);
        EXPECT_EQ(g_control_page[NVIDIA_HOT_SPOT_POINT / 4], expected);
        EXPECT_EQ(g_control_page[NVIDIA_FREE / 4], tried.free);
    }
}
