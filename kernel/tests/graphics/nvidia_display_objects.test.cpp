// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_objects.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_display_objects);

using drivers::nvidia::allocate_display_objects;
using drivers::nvidia::chip_display;
using drivers::nvidia::display_objects;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
using drivers::nvidia::read_display_capabilities;
using drivers::nvidia::release_display_objects;
namespace rm = drivers::nvidia::rm;

// NVIDIA's classes, capability controls, their parameters' sizes and the bits read at their offsets in the parameters,
// restated here so the tests hold the requests to NVIDIA's layouts rather than to the driver's
static constexpr uint32_t NVIDIA_DISPLAY_COMMON_CLASS       = 0x73;
static constexpr uint32_t NVIDIA_BANDWIDTH_CHECK_CLASS      = 0xC372;
static constexpr uint32_t NVIDIA_DISPLAY_CAPABILITIES       = 0x50700709;
static constexpr size_t   NVIDIA_DISPLAY_CAPABILITIES_BYTES = 8;
static constexpr uint32_t NVIDIA_COMMON_CAPABILITIES        = 0x730138;
static constexpr size_t   NVIDIA_COMMON_CAPABILITIES_BYTES  = 2;
static constexpr uint32_t VIDEO_MEMORY_ONLY_OFFSET          = 4;
static constexpr uint8_t  VIDEO_MEMORY_ONLY_BIT             = 0x40;
static constexpr uint32_t CROSSBAR_OFFSET                   = 1;
static constexpr uint8_t  CROSSBAR_BIT                      = 0x08;

// The double's client, device and first handle (unlike those `gsp` gives), and a display class no chip has so the test
// sees it comes from the entry, with the core channel class left 0, as the display's objects never use it
static constexpr rm::handle      TEST_CLIENT  = 0xC1D0000B;
static constexpr rm::handle      TEST_DEVICE  = 0x00050003;
static constexpr rm::handle      FIRST_HANDLE = 0x00050007;
static constexpr chip_display    TEST_DISPLAY = {
    .head_count         = 4,
    .window_count       = 8,
    .display_class      = 0xC67F,
    .core_channel_class = 0,
};

// Handles no call gives, filling `out` before a call so the test sees whether it wrote it
static constexpr display_objects UNTOUCHED = {
    .display_common  = 0xDEAD0001,
    .bandwidth_check = 0xDEAD0002,
    .display         = 0xDEAD0003,
};

// The handles `allocate_display_objects` gives when the double numbers them from `FIRST_HANDLE`
static constexpr display_objects TEST_OBJECTS = {
    .display_common  = FIRST_HANDLE,
    .bandwidth_check = FIRST_HANDLE + 1,
    .display         = FIRST_HANDLE + 2,
};

// A firmware that records its allocations, controls, releases and internal controls in order and answers each with its
// own entry of `results`, and `OK` past them, writing a call's entry of `replies` into its parameters on `OK`
class display_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 6;
    static constexpr uint32_t MAX_BYTES = NVIDIA_DISPLAY_CAPABILITIES_BYTES;

    enum class kind { allocation, control, release, internal_control };

    struct call {
        kind       made;
        rm::handle parent; // 0 but for an allocation
        rm::handle object; // 0 for an internal control
        uint32_t   number; // the class of an allocation or the command of a control, else 0
        bool       has_params;
        size_t     size;
        uint8_t    sent[MAX_BYTES]; // the parameters' first bytes
    };

    call     calls[MAX_CALLS] = {};
    int32_t  results[MAX_CALLS] = {};
    uint8_t  replies[MAX_CALLS][MAX_BYTES] = {};
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

    // No function under test sends one, so a stray one shows in `calls`
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
            .made       = made,
            .parent     = parent,
            .object     = object,
            .number     = number,
            .has_params = params != nullptr,
            .size       = size,
            .sent       = {},
        };

        size_t copied = size < MAX_BYTES ? size : MAX_BYTES;
        if (params != nullptr) {
            string::memcpy(calls[index].sent, params, copied);
        }

        if (results[index] == OK && params != nullptr) {
            string::memcpy(params, replies[index], copied);
        }

        return results[index];
    }
};

// Whether a recorded call is a release of `object`
static bool is_release_of(const display_firmware::call& made, rm::handle object) {
    return made.made == display_firmware::kind::release && made.object == object;
}

// Fills the display's reply with every bit but the one refusing it and display common's with every bit
static void fill_accepted_replies(display_firmware& fw) {
    for (uint32_t i = 0; i < NVIDIA_DISPLAY_CAPABILITIES_BYTES; i++) {
        fw.replies[0][i] = i == VIDEO_MEMORY_ONLY_OFFSET ? static_cast<uint8_t>(~VIDEO_MEMORY_ONLY_BIT) : 0xFF;
    }

    for (uint32_t i = 0; i < NVIDIA_COMMON_CAPABILITIES_BYTES; i++) {
        fw.replies[1][i] = 0xFF;
    }
}

// Display common, the bandwidth check's object and then the display go under the device, each with the next handle,
// its class (the display's from the chip's entry) and no parameters
TEST(nvidia_display_objects, objects_go_under_the_device_in_nvidias_order_without_parameters) {
    display_firmware fw;
    display_objects objects = UNTOUCHED;

    ASSERT_EQ(allocate_display_objects(fw, TEST_DEVICE, TEST_DISPLAY, objects), OK);

    ASSERT_EQ(fw.call_count, 3u);
    const uint32_t classes[] = {NVIDIA_DISPLAY_COMMON_CLASS, NVIDIA_BANDWIDTH_CHECK_CLASS, TEST_DISPLAY.display_class};
    for (uint32_t i = 0; i < 3; i++) {
        const display_firmware::call& allocation = fw.calls[i];
        EXPECT_TRUE(allocation.made == display_firmware::kind::allocation);
        EXPECT_EQ(allocation.parent, TEST_DEVICE);
        EXPECT_EQ(allocation.object, FIRST_HANDLE + i);
        EXPECT_EQ(allocation.number, classes[i]);

        EXPECT_FALSE(allocation.has_params);
        EXPECT_EQ(allocation.size, 0u);
    }

    EXPECT_EQ(objects.display_common, TEST_OBJECTS.display_common);
    EXPECT_EQ(objects.bandwidth_check, TEST_OBJECTS.bandwidth_check);
    EXPECT_EQ(objects.display, TEST_OBJECTS.display);
}

// A failed allocation releases the objects allocated before it in the reverse order and returns its own error
// whatever the releases return, with the handles left as they were
TEST(nvidia_display_objects, failed_allocation_releases_the_earlier_objects_in_reverse) {
    display_firmware failing_common;
    failing_common.results[0] = ERR_TIMEOUT;

    display_firmware failing_bandwidth_check;
    failing_bandwidth_check.results[1] = ERR_TIMEOUT;
    failing_bandwidth_check.results[2] = ERR_FIRMWARE;

    display_firmware failing_display;
    failing_display.results[2] = ERR_TIMEOUT;
    failing_display.results[3] = ERR_FIRMWARE;
    failing_display.results[4] = ERR_FIRMWARE;
    display_objects objects = UNTOUCHED;

    EXPECT_EQ(allocate_display_objects(failing_common, TEST_DEVICE, TEST_DISPLAY, objects), ERR_TIMEOUT);
    EXPECT_EQ(allocate_display_objects(failing_bandwidth_check, TEST_DEVICE, TEST_DISPLAY, objects), ERR_TIMEOUT);
    EXPECT_EQ(allocate_display_objects(failing_display, TEST_DEVICE, TEST_DISPLAY, objects), ERR_TIMEOUT);

    EXPECT_EQ(failing_common.call_count, 1u);
    ASSERT_EQ(failing_bandwidth_check.call_count, 3u);
    EXPECT_TRUE(is_release_of(failing_bandwidth_check.calls[2], TEST_OBJECTS.display_common));
    ASSERT_EQ(failing_display.call_count, 5u);
    EXPECT_TRUE(is_release_of(failing_display.calls[3], TEST_OBJECTS.bandwidth_check));
    EXPECT_TRUE(is_release_of(failing_display.calls[4], TEST_OBJECTS.display_common));

    EXPECT_EQ(objects.display_common, UNTOUCHED.display_common);
    EXPECT_EQ(objects.bandwidth_check, UNTOUCHED.bandwidth_check);
    EXPECT_EQ(objects.display, UNTOUCHED.display);
}

// The display, the bandwidth check's object and display common are released in that order, each even after an
// earlier failure, and the first failure's error is the one returned
TEST(nvidia_display_objects, release_takes_every_object_in_reverse_and_returns_the_first_error) {
    display_firmware accepting;
    display_firmware failing_display;
    failing_display.results[0] = ERR_TIMEOUT;
    failing_display.results[2] = ERR_FIRMWARE;

    display_firmware failing_bandwidth_check;
    failing_bandwidth_check.results[1] = ERR_FIRMWARE;
    failing_bandwidth_check.results[2] = ERR_TIMEOUT;

    display_firmware failing_common;
    failing_common.results[2] = ERR_FIRMWARE;

    EXPECT_EQ(release_display_objects(accepting, TEST_OBJECTS), OK);
    EXPECT_EQ(release_display_objects(failing_display, TEST_OBJECTS), ERR_TIMEOUT);
    EXPECT_EQ(release_display_objects(failing_bandwidth_check, TEST_OBJECTS), ERR_FIRMWARE);
    EXPECT_EQ(release_display_objects(failing_common, TEST_OBJECTS), ERR_FIRMWARE);

    ASSERT_EQ(accepting.call_count, 3u);
    EXPECT_TRUE(is_release_of(accepting.calls[0], TEST_OBJECTS.display));
    EXPECT_TRUE(is_release_of(accepting.calls[1], TEST_OBJECTS.bandwidth_check));
    EXPECT_TRUE(is_release_of(accepting.calls[2], TEST_OBJECTS.display_common));
    EXPECT_EQ(failing_display.call_count, 3u);
    EXPECT_EQ(failing_bandwidth_check.call_count, 3u);
}

// The display is asked first and display common second, each by its own command with its parameters zeroed and at
// NVIDIA's size, and the crossbar bit of display common's reply sets the flag, alone or with every other bit
TEST(nvidia_display_objects, capability_read_asks_the_display_then_display_common_in_nvidias_layout) {
    display_firmware earlier;
    fill_accepted_replies(earlier);
    display_firmware fw;
    fw.replies[1][CROSSBAR_OFFSET] = CROSSBAR_BIT;
    bool earlier_crossbar = false;
    bool has_crossbar = false;

    // The earlier read fills the stack the next read's requests are expected to reuse, so a request not zeroed would
    // likely carry its replies
    ASSERT_EQ(read_display_capabilities(earlier, TEST_OBJECTS, earlier_crossbar), OK);
    ASSERT_EQ(read_display_capabilities(fw, TEST_OBJECTS, has_crossbar), OK);

    ASSERT_EQ(fw.call_count, 2u);
    const display_firmware::call& display = fw.calls[0];
    EXPECT_TRUE(display.made == display_firmware::kind::control);
    EXPECT_EQ(display.object, TEST_OBJECTS.display);
    EXPECT_EQ(display.number, NVIDIA_DISPLAY_CAPABILITIES);

    EXPECT_TRUE(display.has_params);
    EXPECT_EQ(display.size, NVIDIA_DISPLAY_CAPABILITIES_BYTES);
    for (uint32_t i = 0; i < NVIDIA_DISPLAY_CAPABILITIES_BYTES; i++) {
        EXPECT_EQ(display.sent[i], 0u);
    }

    const display_firmware::call& common = fw.calls[1];
    EXPECT_TRUE(common.made == display_firmware::kind::control);
    EXPECT_EQ(common.object, TEST_OBJECTS.display_common);
    EXPECT_EQ(common.number, NVIDIA_COMMON_CAPABILITIES);

    EXPECT_TRUE(common.has_params);
    EXPECT_EQ(common.size, NVIDIA_COMMON_CAPABILITIES_BYTES);
    for (uint32_t i = 0; i < NVIDIA_COMMON_CAPABILITIES_BYTES; i++) {
        EXPECT_EQ(common.sent[i], 0u);
    }

    EXPECT_TRUE(has_crossbar);
    EXPECT_TRUE(earlier_crossbar);
}

// Every bit of display common's reply but the crossbar's leaves the flag clear, with every bit of the display's but
// the one refusing it set
TEST(nvidia_display_objects, only_the_crossbar_bit_sets_the_flag) {
    display_firmware fw;
    fill_accepted_replies(fw);
    fw.replies[1][CROSSBAR_OFFSET] = static_cast<uint8_t>(~CROSSBAR_BIT);
    bool has_crossbar = true;

    ASSERT_EQ(read_display_capabilities(fw, TEST_OBJECTS, has_crossbar), OK);

    EXPECT_FALSE(has_crossbar);
}

// A display whose engine reaches push buffers, semaphores and notifiers only in video memory is refused before display
// common is asked, by its bit alone or with every other bit, with the flag left as it was
TEST(nvidia_display_objects, video_memory_only_display_is_refused) {
    display_firmware bit_alone;
    bit_alone.replies[0][VIDEO_MEMORY_ONLY_OFFSET] = VIDEO_MEMORY_ONLY_BIT;
    bool has_crossbar = true;
    display_firmware every_bit;
    for (uint32_t i = 0; i < NVIDIA_DISPLAY_CAPABILITIES_BYTES; i++) {
        every_bit.replies[0][i] = 0xFF;
    }

    EXPECT_EQ(read_display_capabilities(bit_alone, TEST_OBJECTS, has_crossbar), ERR_BAD_STATE);
    EXPECT_EQ(read_display_capabilities(every_bit, TEST_OBJECTS, has_crossbar), ERR_BAD_STATE);

    EXPECT_EQ(bit_alone.call_count, 1u);
    EXPECT_EQ(every_bit.call_count, 1u);
    EXPECT_TRUE(has_crossbar);
}

// A failed request's error is returned at once, with the flag left as it was
TEST(nvidia_display_objects, failed_capability_requests_are_passed_up) {
    display_firmware failing_display;
    failing_display.results[0] = ERR_TIMEOUT;
    display_firmware failing_common;
    failing_common.results[1] = ERR_FIRMWARE;
    bool has_crossbar = true;

    EXPECT_EQ(read_display_capabilities(failing_display, TEST_OBJECTS, has_crossbar), ERR_TIMEOUT);
    EXPECT_EQ(read_display_capabilities(failing_common, TEST_OBJECTS, has_crossbar), ERR_FIRMWARE);

    EXPECT_EQ(failing_display.call_count, 1u);
    EXPECT_EQ(failing_common.call_count, 2u);
    EXPECT_TRUE(has_crossbar);
}
