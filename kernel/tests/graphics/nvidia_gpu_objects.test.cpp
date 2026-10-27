// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_gpu_objects);

using drivers::nvidia::allocate_gpu_objects;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::gpu_objects;
using drivers::nvidia::OK;
using drivers::nvidia::release_gpu_objects;
namespace rm = drivers::nvidia::rm;

// NVIDIA's classes of a device and a subdevice, their parameters' sizes and the word holding the device's shared
// client, restated here so the tests hold the allocations to NVIDIA's layouts rather than to the driver's
static constexpr uint32_t NVIDIA_DEVICE_CLASS           = 0x80;
static constexpr uint32_t NVIDIA_SUBDEVICE_CLASS        = 0x2080;
static constexpr size_t   NVIDIA_DEVICE_PARAMS_BYTES    = 56;
static constexpr size_t   NVIDIA_SUBDEVICE_PARAMS_BYTES = 4;
static constexpr uint32_t DEVICE_SHARED_CLIENT_WORD     = 1;

// The double's client and first handle (unlike those `gsp` gives), and handles no call gives, filling `out` before a
// call so the test sees whether the call wrote it
static constexpr rm::handle  TEST_CLIENT  = 0xC1D0000B;
static constexpr rm::handle  FIRST_HANDLE = 0x00050007;
static constexpr gpu_objects UNTOUCHED    = {.device = 0xDEAD0001, .subdevice = 0xDEAD0002};

// A firmware that records its allocations and releases in order and answers each with its own entry of `results`,
// counted across every allocation, release and internal control, and `OK` past them
class recording_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 4;
    static constexpr uint32_t MAX_WORDS = NVIDIA_DEVICE_PARAMS_BYTES / 4;

    struct call {
        bool       is_allocation;
        rm::handle parent; // 0 for a release
        rm::handle object;
        uint32_t   class_id; // 0 for a release
        bool       has_params;
        size_t     size;
        uint32_t   words[MAX_WORDS]; // the parameters' first words
    };

    call     calls[MAX_CALLS] = {};
    int32_t  results[MAX_CALLS] = {};
    uint32_t call_count = 0;
    uint32_t handle_count = 0;

    int32_t allocate(rm::handle parent, rm::handle object, uint32_t class_id, void* params, size_t size) override {
        call* recorded = record();
        if (recorded != nullptr) {
            recorded->is_allocation = true;
            recorded->parent = parent;
            recorded->object = object;
            recorded->class_id = class_id;
            recorded->has_params = params != nullptr;
            recorded->size = size;
        }

        if (recorded != nullptr && params != nullptr) {
            string::memcpy(recorded->words, params, size < sizeof(recorded->words) ? size : sizeof(recorded->words));
        }

        return answer();
    }

    int32_t release(rm::handle object) override {
        call* recorded = record();
        if (recorded != nullptr) {
            recorded->object = object;
        }

        return answer();
    }

    rm::handle new_handle() override {
        return FIRST_HANDLE + handle_count++;
    }

    rm::handle client() const override {
        return TEST_CLIENT;
    }

    // No function under test sends one, so a stray one shows in `call_count`
    int32_t internal_control(uint32_t, void*, size_t) override {
        record();
        return answer();
    }

private:
    call* record() {
        call_count++;
        return call_count <= MAX_CALLS ? &calls[call_count - 1] : nullptr;
    }

    int32_t answer() const {
        return call_count <= MAX_CALLS ? results[call_count - 1] : OK;
    }
};

// The device goes under the client with the client shared and every other parameter 0, then the subdevice under the
// device with index 0, each with the next handle
TEST(nvidia_gpu_objects, device_goes_under_the_client_then_the_subdevice_under_the_device) {
    recording_firmware fw;
    gpu_objects objects = UNTOUCHED;

    ASSERT_EQ(allocate_gpu_objects(fw, objects), OK);

    ASSERT_EQ(fw.call_count, 2u);
    const recording_firmware::call& device = fw.calls[0];
    EXPECT_TRUE(device.is_allocation);
    EXPECT_EQ(device.parent, TEST_CLIENT);
    EXPECT_EQ(device.object, FIRST_HANDLE);
    EXPECT_EQ(device.class_id, NVIDIA_DEVICE_CLASS);
    EXPECT_TRUE(device.has_params);
    EXPECT_EQ(device.size, NVIDIA_DEVICE_PARAMS_BYTES);
    for (uint32_t i = 0; i < recording_firmware::MAX_WORDS; i++) {
        EXPECT_EQ(device.words[i], i == DEVICE_SHARED_CLIENT_WORD ? TEST_CLIENT : 0u);
    }

    const recording_firmware::call& subdevice = fw.calls[1];
    EXPECT_TRUE(subdevice.is_allocation);
    EXPECT_EQ(subdevice.parent, FIRST_HANDLE);
    EXPECT_EQ(subdevice.object, FIRST_HANDLE + 1);
    EXPECT_EQ(subdevice.class_id, NVIDIA_SUBDEVICE_CLASS);
    EXPECT_TRUE(subdevice.has_params);
    EXPECT_EQ(subdevice.size, NVIDIA_SUBDEVICE_PARAMS_BYTES);
    EXPECT_EQ(subdevice.words[0], 0u);

    EXPECT_EQ(objects.device, FIRST_HANDLE);
    EXPECT_EQ(objects.subdevice, FIRST_HANDLE + 1);
}

// A failed device stops the allocations at once, with its error returned and the handles left as they were
TEST(nvidia_gpu_objects, failed_device_allocates_nothing_more) {
    recording_firmware fw;
    fw.results[0] = ERR_TIMEOUT;
    gpu_objects objects = UNTOUCHED;

    EXPECT_EQ(allocate_gpu_objects(fw, objects), ERR_TIMEOUT);

    EXPECT_EQ(fw.call_count, 1u);
    EXPECT_EQ(objects.device, UNTOUCHED.device);
    EXPECT_EQ(objects.subdevice, UNTOUCHED.subdevice);
}

// A failed subdevice releases the device, with the subdevice's error returned whatever the release returns and the
// handles left as they were
TEST(nvidia_gpu_objects, failed_subdevice_releases_the_device) {
    recording_firmware fw;
    fw.results[1] = ERR_TIMEOUT;
    fw.results[2] = ERR_FIRMWARE;
    gpu_objects objects = UNTOUCHED;

    EXPECT_EQ(allocate_gpu_objects(fw, objects), ERR_TIMEOUT);

    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_FALSE(fw.calls[2].is_allocation);
    EXPECT_EQ(fw.calls[2].object, FIRST_HANDLE);
    EXPECT_EQ(objects.device, UNTOUCHED.device);
    EXPECT_EQ(objects.subdevice, UNTOUCHED.subdevice);
}

// The subdevice is released before the device, and the device even after a failed subdevice, whose error is the one
// returned whatever the device's, while a failed device alone returns its own
TEST(nvidia_gpu_objects, release_takes_the_subdevice_then_the_device_whatever_the_first_returns) {
    gpu_objects objects = {.device = FIRST_HANDLE, .subdevice = FIRST_HANDLE + 1};
    recording_firmware accepting;
    recording_firmware failing_subdevice;
    failing_subdevice.results[0] = ERR_TIMEOUT;
    recording_firmware failing_device;
    failing_device.results[1] = ERR_FIRMWARE;
    recording_firmware failing_both;
    failing_both.results[0] = ERR_TIMEOUT;
    failing_both.results[1] = ERR_FIRMWARE;

    EXPECT_EQ(release_gpu_objects(accepting, objects), OK);
    EXPECT_EQ(release_gpu_objects(failing_subdevice, objects), ERR_TIMEOUT);
    EXPECT_EQ(release_gpu_objects(failing_device, objects), ERR_FIRMWARE);
    EXPECT_EQ(release_gpu_objects(failing_both, objects), ERR_TIMEOUT);

    ASSERT_EQ(accepting.call_count, 2u);
    EXPECT_FALSE(accepting.calls[0].is_allocation);
    EXPECT_EQ(accepting.calls[0].object, FIRST_HANDLE + 1);
    EXPECT_FALSE(accepting.calls[1].is_allocation);
    EXPECT_EQ(accepting.calls[1].object, FIRST_HANDLE);
    ASSERT_EQ(failing_subdevice.call_count, 2u);
    EXPECT_EQ(failing_subdevice.calls[1].object, FIRST_HANDLE);
    EXPECT_EQ(failing_both.call_count, 2u);
}
