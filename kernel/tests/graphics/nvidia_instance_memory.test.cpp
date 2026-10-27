// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/vram_allocator.h"

TEST_SUITE(nvidia_instance_memory);

using drivers::nvidia::cpu_mapping;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_QUEUE_FULL;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::ERR_TOO_LARGE;
using drivers::nvidia::firmware;
using drivers::nvidia::instance_memory;
using drivers::nvidia::OK;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
namespace rm = drivers::nvidia::rm;

// NVIDIA's size and alignment of the display's instance memory, and its control's command, parameter bytes, address
// space and caching, restated here so the tests hold the driver to NVIDIA's values rather than to its own
static constexpr uint64_t NVIDIA_INSTANCE_MEMORY_BYTES     = 0x10000;
static constexpr uint64_t NVIDIA_INSTANCE_MEMORY_ALIGNMENT = 0x10000;
static constexpr uint32_t NVIDIA_WRITE_INST_MEM            = 0x20800A49;
static constexpr size_t   WRITE_INST_MEM_BYTES             = 24;
static constexpr uint32_t WRITE_INST_MEM_WORDS             = WRITE_INST_MEM_BYTES / 4;
static constexpr uint32_t NVIDIA_ADDR_FBMEM                = 2;
static constexpr uint32_t NVIDIA_MEMORY_WRITECOMBINED      = 2;

// A range above 4 GiB, so a request carrying only the address's low word is caught, and the byte the memory holds
// before `set_up`, so a test sees whether it was cleared
static constexpr vram_range HANDED_OUT    = {.offset = 0x1'0007'0000, .size = NVIDIA_INSTANCE_MEMORY_BYTES};
static constexpr uint8_t    STALE_PATTERN = 0xA5;

// The memory the video memory double maps, in words so a mapping of it is aligned for every write, with a guard past
// the mapped bytes that a clear running past them would overwrite
static constexpr uint64_t INSTANCE_MEMORY_WORDS = NVIDIA_INSTANCE_MEMORY_BYTES / sizeof(uint64_t);
static constexpr uint64_t GUARD_WORDS           = 512;
static uint64_t           g_backing[INSTANCE_MEMORY_WORDS + GUARD_WORDS];

// A video memory that hands out `HANDED_OUT` and maps it over `g_backing`, recording each call and whether the memory
// was cleared when it was unmapped, and refuses `allocate` or `map_for_cpu` with the result set for it
class recording_video_memory : public video_memory {
public:
    int32_t    allocate_result = OK;
    int32_t    map_result = OK;
    uint32_t   allocate_count = 0;
    uint64_t   allocated_size = 0;
    uint64_t   allocated_alignment = 0;
    vram_use   allocated_use = vram_use::scanout;
    uint32_t   map_count = 0;
    vram_range mapped = {};
    uint32_t   release_count = 0;
    vram_range released = {};
    uint32_t   unmap_count = 0;
    vram_range unmapped = {};
    bool       is_cleared_at_unmap = false;

    recording_video_memory() {
        string::memset(g_backing, STALE_PATTERN, sizeof(g_backing));
    }

    static bool is_cleared() {
        for (uint64_t i = 0; i < INSTANCE_MEMORY_WORDS; i++) {
            if (g_backing[i] != 0) {
                return false;
            }
        }

        return true;
    }

    static bool is_guard_intact() {
        uint64_t stale_word = 0;
        string::memset(&stale_word, STALE_PATTERN, sizeof(stale_word));
        for (uint64_t i = INSTANCE_MEMORY_WORDS; i < INSTANCE_MEMORY_WORDS + GUARD_WORDS; i++) {
            if (g_backing[i] != stale_word) {
                return false;
            }
        }

        return true;
    }

    int32_t allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) override {
        allocate_count++;
        allocated_size = size;
        allocated_alignment = alignment;
        allocated_use = use;
        if (allocate_result != OK) {
            return allocate_result;
        }

        out = HANDED_OUT;
        return OK;
    }

    void release(const vram_range& range) override {
        release_count++;
        released = range;
    }

    int32_t map_for_cpu(const vram_range& range, cpu_mapping& out) override {
        map_count++;
        mapped = range;
        if (map_result != OK) {
            return map_result;
        }

        out.virt = reinterpret_cast<uintptr_t>(g_backing);
        out.phys = 0;
        return OK;
    }

    void unmap_for_cpu(const vram_range& range) override {
        unmap_count++;
        unmapped = range;
        is_cleared_at_unmap = is_cleared();
    }
};

// A firmware that answers internal controls with `result`, recording what each carried and how many unmaps `memory`
// had seen by then, and counts the calls to `allocate`, `release` and `new_handle`, which `set_up` never makes
class instance_memory_firmware : public firmware {
public:
    const recording_video_memory& memory;
    int32_t                       result = OK;
    uint32_t                      control_count = 0;
    uint32_t                      command = 0;
    size_t                        size = 0;
    uint32_t                      sent[WRITE_INST_MEM_WORDS] = {};
    uint32_t                      unmaps_at_control = 0;
    uint32_t                      other_count = 0;

    explicit instance_memory_firmware(const recording_video_memory& watched) : memory(watched) {}

    int32_t internal_control(uint32_t command_sent, void* params, size_t params_size) override {
        control_count++;
        command = command_sent;
        size = params_size;
        unmaps_at_control = memory.unmap_count;
        if (params != nullptr) {
            size_t copied = params_size < sizeof(sent) ? params_size : sizeof(sent);
            string::memcpy(sent, params, copied);
        }

        return result;
    }

    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override {
        other_count++;
        return OK;
    }

    int32_t release(rm::handle) override {
        other_count++;
        return OK;
    }

    rm::handle new_handle() override {
        other_count++;
        return 0;
    }

    rm::handle client() const override {
        return 0;
    }
};

// The memory is NVIDIA's 64 KiB at NVIDIA's alignment, cleared and unmapped before its address goes out in one request
// in NVIDIA's layout as video memory mapped write-combined, after which the object holds the range
TEST(nvidia_instance_memory, set_up_clears_the_memory_and_sends_its_address_in_nvidias_layout) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory instance(memory, fw);
    EXPECT_EQ(instance.range().size, 0u);

    int32_t rc = instance.set_up();

    EXPECT_EQ(rc, OK);
    EXPECT_EQ(memory.allocate_count, 1u);
    EXPECT_EQ(memory.allocated_size, NVIDIA_INSTANCE_MEMORY_BYTES);
    EXPECT_EQ(memory.allocated_alignment, NVIDIA_INSTANCE_MEMORY_ALIGNMENT);
    EXPECT_TRUE(memory.allocated_use == vram_use::general);

    EXPECT_EQ(memory.map_count, 1u);
    EXPECT_EQ(memory.mapped.offset, HANDED_OUT.offset);
    EXPECT_EQ(memory.mapped.size, HANDED_OUT.size);
    EXPECT_EQ(memory.release_count, 0u);

    EXPECT_EQ(memory.unmap_count, 1u);
    EXPECT_EQ(memory.unmapped.offset, HANDED_OUT.offset);
    EXPECT_EQ(memory.unmapped.size, HANDED_OUT.size);
    EXPECT_TRUE(memory.is_cleared_at_unmap);
    EXPECT_TRUE(memory.is_guard_intact());

    EXPECT_EQ(fw.control_count, 1u);
    EXPECT_EQ(fw.unmaps_at_control, 1u);
    EXPECT_EQ(fw.other_count, 0u);
    EXPECT_EQ(fw.command, NVIDIA_WRITE_INST_MEM);
    EXPECT_EQ(fw.size, WRITE_INST_MEM_BYTES);

    EXPECT_EQ(fw.sent[0], static_cast<uint32_t>(HANDED_OUT.offset));
    EXPECT_EQ(fw.sent[1], static_cast<uint32_t>(HANDED_OUT.offset >> 32));
    EXPECT_EQ(fw.sent[2], static_cast<uint32_t>(NVIDIA_INSTANCE_MEMORY_BYTES));
    EXPECT_EQ(fw.sent[3], 0u);
    EXPECT_EQ(fw.sent[4], NVIDIA_ADDR_FBMEM);
    EXPECT_EQ(fw.sent[5], NVIDIA_MEMORY_WRITECOMBINED);

    EXPECT_EQ(instance.range().offset, HANDED_OUT.offset);
    EXPECT_EQ(instance.range().size, HANDED_OUT.size);
}

// Every error `video_memory::allocate` returns
static constexpr int32_t ALLOCATE_ERRORS[] = {ERR_INVALID_ARG, ERR_NO_MEMORY};

// With no memory handed out there is nothing to map, release or send
TEST(nvidia_instance_memory, a_failed_allocation_maps_releases_and_sends_nothing) {
    for (int32_t error : ALLOCATE_ERRORS) {
        recording_video_memory memory;
        memory.allocate_result = error;
        instance_memory_firmware fw(memory);
        instance_memory instance(memory, fw);

        int32_t rc = instance.set_up();

        EXPECT_EQ(rc, error);
        EXPECT_EQ(memory.map_count, 0u);
        EXPECT_EQ(memory.release_count, 0u);
        EXPECT_EQ(fw.control_count, 0u);
        EXPECT_EQ(instance.range().size, 0u);
    }
}

// Every error `video_memory::map_for_cpu` returns
static constexpr int32_t MAP_ERRORS[] = {ERR_INVALID_ARG, ERR_NO_MEMORY, ERR_BAD_STATE, ERR_TIMEOUT};

// The firmware was never told of the memory, so a failed mapping gives the memory back and leaves no range held
TEST(nvidia_instance_memory, a_failed_mapping_releases_the_memory_and_sends_nothing) {
    for (int32_t error : MAP_ERRORS) {
        recording_video_memory memory;
        memory.map_result = error;
        instance_memory_firmware fw(memory);
        instance_memory instance(memory, fw);

        int32_t rc = instance.set_up();

        EXPECT_EQ(rc, error);
        EXPECT_EQ(memory.unmap_count, 0u);
        EXPECT_EQ(memory.release_count, 1u);
        EXPECT_EQ(memory.released.offset, HANDED_OUT.offset);
        EXPECT_EQ(memory.released.size, HANDED_OUT.size);

        EXPECT_EQ(fw.control_count, 0u);
        EXPECT_EQ(instance.range().size, 0u);
    }
}

// Every error `firmware::internal_control` returns
static constexpr int32_t CONTROL_ERRORS[] = {
    ERR_TOO_LARGE,
    ERR_NO_MEMORY,
    ERR_QUEUE_FULL,
    ERR_TIMEOUT,
    ERR_FIRMWARE,
    ERR_BAD_MESSAGE,
};

// Every error keeps the memory allocated and held, including those raised before the request is sent
TEST(nvidia_instance_memory, a_failed_control_keeps_the_memory_after_every_error) {
    for (int32_t error : CONTROL_ERRORS) {
        recording_video_memory memory;
        instance_memory_firmware fw(memory);
        fw.result = error;
        instance_memory instance(memory, fw);

        int32_t rc = instance.set_up();

        EXPECT_EQ(rc, error);
        EXPECT_EQ(fw.control_count, 1u);
        EXPECT_EQ(memory.release_count, 0u);
        EXPECT_EQ(instance.range().offset, HANDED_OUT.offset);
        EXPECT_EQ(instance.range().size, HANDED_OUT.size);
    }
}
