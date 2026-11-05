// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/address_space.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_address_space);

using drivers::nvidia::address_space;
using drivers::nvidia::context_tlbs;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::failed_mark;
using drivers::nvidia::firmware;
using drivers::nvidia::gpu_objects;
using drivers::nvidia::OK;
using drivers::nvidia::table_writes;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
namespace rm = drivers::nvidia::rm;

// NVIDIA's classes, controls and parameter words, restated here so the tests hold the calls to NVIDIA's layout rather
// than to the driver's
static constexpr uint32_t FERMI_VASPACE_A                    = 0x90F1;
static constexpr size_t   VASPACE_PARAMS_BYTES               = 48;
static constexpr uint32_t VASPACE_FLAGS_WORD                 = 1;
static constexpr uint32_t VASPACE_IS_EXTERNALLY_OWNED        = 0x8;
static constexpr uint32_t SET_PAGE_DIRECTORY                 = 0x801813;
static constexpr size_t   SET_PAGE_DIRECTORY_BYTES           = 32;
static constexpr uint32_t UNSET_PAGE_DIRECTORY               = 0x801814;
static constexpr size_t   UNSET_PAGE_DIRECTORY_BYTES         = 8;
static constexpr uint32_t DIRECTORY_FLAGS_VIDMEM_ALL_CHANNELS = 0x8;

static constexpr gpu_objects TEST_OBJECTS = {.device = 0xC1D00001, .subdevice = 0xC1D00002};
static constexpr rm::handle  FIRST_HANDLE = 0xC1D00010;
static constexpr uint64_t    ROOT_OFFSET  = 0x400000;

// A firmware recording each call's kind, objects, command or class, size and first 8 parameter words, answering call
// `i` with `results[i]`
class address_space_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 8;
    static constexpr uint32_t MAX_WORDS = 8;

    enum class kind : uint8_t { allocate, control, release };

    struct call {
        kind       what;
        rm::handle parent;
        rm::handle object;
        uint32_t   number;
        size_t     size;
        uint32_t   words[MAX_WORDS];
    };

    call       calls[MAX_CALLS] = {};
    uint32_t   call_count = 0;
    int32_t    results[MAX_CALLS] = {};
    rm::handle next_handle = FIRST_HANDLE;

    int32_t allocate(rm::handle parent, rm::handle object, uint32_t class_id, void* params, size_t size) override {
        return record(kind::allocate, parent, object, class_id, params, size);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        return record(kind::control, 0, object, command, params, size);
    }

    int32_t release(rm::handle object) override {
        return record(kind::release, 0, object, 0, nullptr, 0);
    }

    rm::handle new_handle() override {
        return next_handle++;
    }

    rm::handle client() const override {
        return 0;
    }

    int32_t internal_control(uint32_t, void*, size_t) override {
        return OK;
    }

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

private:
    int32_t record(kind what, rm::handle parent, rm::handle object, uint32_t number, void* params, size_t size) {
        uint32_t index = call_count++;
        if (index >= MAX_CALLS) {
            return ERR_FIRMWARE;
        }

        call& recorded = calls[index];
        recorded = {.what = what, .parent = parent, .object = object, .number = number, .size = size, .words = {}};
        if (params != nullptr) {
            size_t bytes = size < sizeof(recorded.words) ? size : sizeof(recorded.words);
            string::memcpy(recorded.words, params, bytes);
        }

        return results[index];
    }
};

// A video memory handing out page tables from `ROOT_OFFSET` on and accepting every update, counting releases
class counting_video_memory : public video_memory {
public:
    uint64_t next_offset = ROOT_OFFSET;
    uint32_t releases = 0;

    int32_t allocate(uint64_t size, uint64_t, vram_use, vram_range& out) override {
        out = {.offset = next_offset, .size = size};
        next_offset += size;
        return OK;
    }

    void release(const vram_range&) override { releases++; }

    int32_t map_for_cpu(const vram_range&, cpu_mapping&) override { return ERR_INVALID_ARG; }

    void unmap_for_cpu(const vram_range&) override {}

    int32_t update_context_tables(uint64_t, table_writes&, context_tlbs) override { return OK; }
};

// Create allocates the address space externally owned under the device, then hands the device's control the root
// directory's video memory offset, its 4 entries in use, both flags and subdevice 1
TEST(nvidia_address_space, create_hands_the_root_directory_to_the_firmware) {
    address_space_firmware fw;
    counting_video_memory memory;
    failed_mark failed;
    address_space space(fw, TEST_OBJECTS, memory, failed);

    ASSERT_EQ(space.create(), OK);

    ASSERT_EQ(fw.call_count, 2u);

    const address_space_firmware::call& allocation = fw.calls[0];
    EXPECT_TRUE(allocation.what == address_space_firmware::kind::allocate);
    EXPECT_EQ(allocation.parent, TEST_OBJECTS.device);
    EXPECT_EQ(allocation.object, FIRST_HANDLE);
    EXPECT_EQ(allocation.number, FERMI_VASPACE_A);
    EXPECT_EQ(allocation.size, VASPACE_PARAMS_BYTES);

    for (uint32_t i = 0; i < address_space_firmware::MAX_WORDS; i++) {
        EXPECT_EQ(allocation.words[i], i == VASPACE_FLAGS_WORD ? VASPACE_IS_EXTERNALLY_OWNED : 0u);
    }

    const address_space_firmware::call& directory = fw.calls[1];
    EXPECT_TRUE(directory.what == address_space_firmware::kind::control);
    EXPECT_EQ(directory.object, TEST_OBJECTS.device);
    EXPECT_EQ(directory.number, SET_PAGE_DIRECTORY);
    EXPECT_EQ(directory.size, SET_PAGE_DIRECTORY_BYTES);

    EXPECT_EQ(directory.words[0], static_cast<uint32_t>(ROOT_OFFSET));
    EXPECT_EQ(directory.words[1], 0u);
    EXPECT_EQ(directory.words[2], 4u);
    EXPECT_EQ(directory.words[3], DIRECTORY_FLAGS_VIDMEM_ALL_CHANNELS);

    EXPECT_EQ(directory.words[4], FIRST_HANDLE);
    EXPECT_EQ(directory.words[5], 0u);
    EXPECT_EQ(directory.words[6], 1u);
    EXPECT_EQ(directory.words[7], 0u);

    EXPECT_EQ(space.handle(), FIRST_HANDLE);
    EXPECT_EQ(space.create(), ERR_BAD_STATE);
}

// A refused allocation destroys the page tables and leaves the object empty
TEST(nvidia_address_space, refused_allocation_destroys_the_tables) {
    address_space_firmware fw;
    fw.results[0] = ERR_NO_MEMORY;
    counting_video_memory memory;
    failed_mark failed;
    address_space space(fw, TEST_OBJECTS, memory, failed);

    EXPECT_EQ(space.create(), ERR_NO_MEMORY);

    EXPECT_EQ(fw.call_count, 1u);
    EXPECT_EQ(memory.releases, 1u);
    EXPECT_EQ(space.handle(), 0u);
}

// A refused directory releases the firmware's object and destroys the page tables
TEST(nvidia_address_space, refused_directory_releases_the_object_and_the_tables) {
    address_space_firmware fw;
    fw.results[1] = ERR_FIRMWARE;
    counting_video_memory memory;
    failed_mark failed;
    address_space space(fw, TEST_OBJECTS, memory, failed);

    EXPECT_EQ(space.create(), ERR_FIRMWARE);

    ASSERT_EQ(fw.call_count, 3u);
    EXPECT_TRUE(fw.calls[2].what == address_space_firmware::kind::release);
    EXPECT_EQ(fw.calls[2].object, FIRST_HANDLE);
    EXPECT_EQ(memory.releases, 1u);
    EXPECT_EQ(space.handle(), 0u);
}

// Destroy takes the directory back through the device, releases the object and frees the tables
TEST(nvidia_address_space, destroy_takes_the_directory_back_and_frees_the_tables) {
    address_space_firmware fw;
    counting_video_memory memory;
    failed_mark failed;
    address_space space(fw, TEST_OBJECTS, memory, failed);
    ASSERT_EQ(space.create(), OK);

    EXPECT_EQ(space.destroy(), OK);

    ASSERT_EQ(fw.call_count, 4u);

    const address_space_firmware::call& directory = fw.calls[2];
    EXPECT_TRUE(directory.what == address_space_firmware::kind::control);
    EXPECT_EQ(directory.object, TEST_OBJECTS.device);
    EXPECT_EQ(directory.number, UNSET_PAGE_DIRECTORY);
    EXPECT_EQ(directory.size, UNSET_PAGE_DIRECTORY_BYTES);

    EXPECT_EQ(directory.words[0], FIRST_HANDLE);
    EXPECT_EQ(directory.words[1], 1u);

    EXPECT_TRUE(fw.calls[3].what == address_space_firmware::kind::release);
    EXPECT_EQ(fw.calls[3].object, FIRST_HANDLE);
    EXPECT_EQ(memory.releases, 1u);

    EXPECT_EQ(space.handle(), 0u);
    EXPECT_EQ(space.destroy(), ERR_BAD_STATE);
}

// A refused take-back keeps the object and the tables, since the firmware may still walk them
TEST(nvidia_address_space, refused_take_back_keeps_the_tables) {
    address_space_firmware fw;
    fw.results[2] = ERR_FIRMWARE;
    counting_video_memory memory;
    failed_mark failed;
    address_space space(fw, TEST_OBJECTS, memory, failed);
    ASSERT_EQ(space.create(), OK);

    EXPECT_EQ(space.destroy(), ERR_FIRMWARE);

    EXPECT_EQ(fw.call_count, 3u);
    EXPECT_EQ(memory.releases, 0u);
    EXPECT_EQ(space.handle(), FIRST_HANDLE);
}

// A release refused after the take-back still frees the tables, since the firmware walks its own directory again, and
// leaves the object in the firmware
TEST(nvidia_address_space, refused_release_after_the_take_back_frees_the_tables) {
    address_space_firmware fw;
    fw.results[3] = ERR_FIRMWARE;
    counting_video_memory memory;
    failed_mark failed;
    address_space space(fw, TEST_OBJECTS, memory, failed);
    ASSERT_EQ(space.create(), OK);

    EXPECT_EQ(space.destroy(), ERR_FIRMWARE);

    EXPECT_EQ(fw.call_count, 4u);
    EXPECT_EQ(memory.releases, 1u);
    EXPECT_EQ(space.handle(), 0u);
}
