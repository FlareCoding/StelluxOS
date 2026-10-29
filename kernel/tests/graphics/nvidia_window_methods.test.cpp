// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/window_methods.h"

TEST_SUITE(nvidia_window_methods);

using drivers::nvidia::build_window_image_methods;
using drivers::nvidia::window_image;
using drivers::nvidia::window_image_methods;
using drivers::nvidia::WINDOW_IMAGE_METHODS;
using drivers::nvidia::window_image_update;

// A method and its one value as NVIDIA's flip writes it, restated so the tests hold the methods to NVIDIA's offsets and
// values rather than to the driver's own
struct nvidia_method {
    uint32_t method;
    uint32_t value;
};

// A 2560 by 1440 image on window 2, its surface's and look-up table's context DMA handles, past 16 bits as the driver's
// are, and its pitch, 10,240 bytes
static constexpr uint8_t  IMAGE_WINDOW   = 2;
static constexpr uint32_t SURFACE_HANDLE = 0x0001A1A1;
static constexpr uint32_t TABLE_HANDLE   = 0x0002B2B2;
static constexpr uint32_t IMAGE_WIDTH    = 2560;
static constexpr uint32_t IMAGE_HEIGHT   = 1440;
static constexpr uint32_t IMAGE_PITCH    = 10240;

// The window notifier's context DMA handle a present names, past 16 bits as the driver's are
static constexpr uint32_t NOTIFIER_HANDLE = 0x0003C3C3;

// The update timestamp's place among the methods, the one method carrying two values, NVIDIA's _LO then _HI
static constexpr uint32_t TIMESTAMP_INDEX = 23;

// A value no method is given, so a lookup finding no method is seen
static constexpr uint32_t NO_METHOD = 0xDEADBEEF;

// The light's image of window 2, interlocked with the core channel, as NVIDIA's flip writes it: each method's offset
// and value in order from the scan direction to the update, the timestamp by its first value
static constexpr nvidia_method LIGHT_IMAGE[WINDOW_IMAGE_METHODS] = {
    {0x33C, 0},
    {0x21C, 0},
    {0x220, 0},
    {0x308, 0x1},
    {0x240, SURFACE_HANDLE},
    {0x260, 0},
    {0x248, 0},
    {0x268, 0},
    {0x250, 0},
    {0x270, 0},
    {0x244, 0},
    {0x264, 0},
    {0x24C, 0},
    {0x26C, 0},
    {0x254, 0},
    {0x274, 0},
    {0x224, 0x05A00A00},
    {0x298, 0x05A00A00},
    {0x2A4, 0x05A00A00},
    {0x228, 0},
    {0x230, 0xA0},
    {0x234, 0},
    {0x238, 0},
    {0x348, 0},
    {0x22C, 0xE6},
    {0x400, 0x10000},
    {0x404, 0},
    {0x408, 0},
    {0x40C, 0},
    {0x410, 0},
    {0x414, 0x10000},
    {0x418, 0},
    {0x41C, 0},
    {0x420, 0},
    {0x424, 0},
    {0x428, 0x10000},
    {0x42C, 0},
    {0x2A8, 0x11},
    {0x59C, 0},
    {0x440, 0x00040508},
    {0x448, 0},
    {0x444, TABLE_HANDLE},
    {0x2EC, 0x80},
    {0x2F0, 0},
    {0x2F4, 0x11},
    {0x338, 0},
    {0x330, 0},
    {0x334, 0},
    {0x218, 0},
    {0x20C, 0},
    {0x214, 0},
    {0x370, 0x1},
    {0x374, 1u << IMAGE_WINDOW},
    {0x200, 0x1},
};

// Gives the 2560 by 1440 image on window 2
static window_image light_image() {
    return {
        .window = IMAGE_WINDOW,
        .surface_context_dma = SURFACE_HANDLE,
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .pitch = IMAGE_PITCH,
        .look_up_table_context_dma = TABLE_HANDLE,
    };
}

// Gives the light's update: no notifier, interlocked with the core channel
static window_image_update light_update() {
    return {.notifier_context_dma = 0, .notifier_slot = 0, .interlocks_core = true};
}

// Gives the first value of the first of `methods` at register offset `method`, or `NO_METHOD` when none is
static uint32_t value_of(const window_image_methods& methods, uint32_t method) {
    for (uint32_t index = 0; index < WINDOW_IMAGE_METHODS; index++) {
        if (methods.methods[index].method == method) {
            return methods.methods[index].values[0];
        }
    }

    return NO_METHOD;
}

// The light's image, interlocked with the core channel and naming no notifier, is NVIDIA's flip of a 2560 by 1440
// surface in order from the scan direction to the update, each method with one value but the timestamp's two zeros
TEST(nvidia_window_methods, the_lights_image_is_nvidias_flip_in_order) {
    window_image_methods methods = {};
    build_window_image_methods(light_image(), light_update(), methods);

    for (uint32_t index = 0; index < WINDOW_IMAGE_METHODS; index++) {
        uint32_t expected_count = index == TIMESTAMP_INDEX ? 2 : 1;

        EXPECT_EQ(methods.methods[index].method, LIGHT_IMAGE[index].method);
        EXPECT_EQ(methods.methods[index].values[0], LIGHT_IMAGE[index].value);
        EXPECT_EQ(methods.methods[index].count, expected_count);
    }

    EXPECT_EQ(methods.methods[TIMESTAMP_INDEX].values[1], 0u);
}

// With their headers the methods take 109 words of a push buffer: a header and a value for each method, and the
// timestamp's second value
TEST(nvidia_window_methods, the_methods_take_109_words) {
    window_image_methods methods = {};
    build_window_image_methods(light_image(), light_update(), methods);

    uint32_t words = 0;
    for (uint32_t index = 0; index < WINDOW_IMAGE_METHODS; index++) {
        words += 1 + methods.methods[index].count;
    }

    EXPECT_EQ(words, 109u);
}

// Gives the value a present's image writes at register offset `method`, where the light's image writes `light_value`:
// the same but for the notifier's context DMA `NOTIFIER_HANDLE`, its slot 3 in 16-byte units and no core interlock
static uint32_t present_value(uint32_t method, uint32_t light_value) {
    if (method == 0x21C) {
        return NOTIFIER_HANDLE;
    }

    if (method == 0x220) {
        return 0x30;
    }

    if (method == 0x370) {
        return 0;
    }

    return light_value;
}

// A present's image is the light's in the same order but for its notifier, named by its slot in 16-byte units with
// the write mode, and its interlock, which leaves the core channel out and keeps its own window's
TEST(nvidia_window_methods, a_present_names_its_notifier_slot_and_no_core_interlock) {
    window_image_update update = {
        .notifier_context_dma = NOTIFIER_HANDLE,
        .notifier_slot = 3,
        .interlocks_core = false,
    };

    window_image_methods methods = {};
    build_window_image_methods(light_image(), update, methods);

    for (uint32_t index = 0; index < WINDOW_IMAGE_METHODS; index++) {
        uint32_t method = LIGHT_IMAGE[index].method;
        uint32_t expected_count = index == TIMESTAMP_INDEX ? 2 : 1;

        EXPECT_EQ(methods.methods[index].method, method);
        EXPECT_EQ(methods.methods[index].values[0], present_value(method, LIGHT_IMAGE[index].value));
        EXPECT_EQ(methods.methods[index].count, expected_count);
    }

    EXPECT_EQ(methods.methods[TIMESTAMP_INDEX].values[1], 0u);
}

// A slot given with no notifier's context DMA leaves the notifier's control 0, as NVIDIA writes it for none
TEST(nvidia_window_methods, no_notifier_ignores_the_slot) {
    window_image_update update = {.notifier_context_dma = 0, .notifier_slot = 3, .interlocks_core = true};
    window_image_methods methods = {};
    build_window_image_methods(light_image(), update, methods);

    EXPECT_EQ(value_of(methods, 0x21C), 0u);
    EXPECT_EQ(value_of(methods, 0x220), 0u);
}

// A 1920 by 1080 image on window 5 gives its size with the height in the top 16 bits, its 16384-byte pitch, padded
// past the 7680 bytes of a row's pixels, as 256 units of 64 bytes, and its own window's interlock bit
TEST(nvidia_window_methods, the_size_pitch_and_interlock_follow_the_image) {
    window_image image = light_image();
    image.window = 5;
    image.width = 1920;
    image.height = 1080;
    image.pitch = 16384;

    window_image_methods methods = {};
    build_window_image_methods(image, light_update(), methods);

    EXPECT_EQ(value_of(methods, 0x224), 0x04380780u);
    EXPECT_EQ(value_of(methods, 0x298), 0x04380780u);
    EXPECT_EQ(value_of(methods, 0x2A4), 0x04380780u);
    EXPECT_EQ(value_of(methods, 0x230), 0x100u);
    EXPECT_EQ(value_of(methods, 0x374), 1u << 5);
}

// The largest values within the bounds, a 65535 by 65535 image on window 31 at a pitch of 524,224 bytes and slot 255,
// fill their fields whole: the size's 16 bits each way, the pitch's 13, the interlock's top bit and the slot's 8
TEST(nvidia_window_methods, the_largest_values_fill_their_fields) {
    window_image image = light_image();
    image.window = 31;
    image.width = 65535;
    image.height = 65535;
    image.pitch = 524224;

    window_image_update update = light_update();
    update.notifier_context_dma = NOTIFIER_HANDLE;
    update.notifier_slot = 255;

    window_image_methods methods = {};
    build_window_image_methods(image, update, methods);

    EXPECT_EQ(value_of(methods, 0x224), 0xFFFFFFFFu);
    EXPECT_EQ(value_of(methods, 0x298), 0xFFFFFFFFu);
    EXPECT_EQ(value_of(methods, 0x2A4), 0xFFFFFFFFu);
    EXPECT_EQ(value_of(methods, 0x230), 0x1FFFu);
    EXPECT_EQ(value_of(methods, 0x374), 0x80000000u);
    EXPECT_EQ(value_of(methods, 0x220), 0xFF0u);
}
