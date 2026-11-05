// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "mm/page_quarantine.h"
#include "mm/pmm.h"
#include "tests/graphics/nvidia_channel_test_access.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_channel);

using drivers::nvidia::address_space;
using drivers::nvidia::channel;
using drivers::nvidia::channel_allocation_flags;
using drivers::nvidia::channel_setup;
using drivers::nvidia::chip_info;
using drivers::nvidia::context_tlbs;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_GPU_FAILED;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::failed_mark;
using drivers::nvidia::firmware;
using drivers::nvidia::GPFIFO_ENTRIES;
using drivers::nvidia::gpfifo_entry;
using drivers::nvidia::gpu_objects;
using drivers::nvidia::OK;
using drivers::nvidia::registers;
using drivers::nvidia::table_writes;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
namespace rm = drivers::nvidia::rm;

using drivers::nvidia::channel_test_access;

// NVIDIA's classes, controls, parameter layouts, USERD offset and doorbell, restated here so the tests hold the calls
// to NVIDIA's layout rather than to the driver's
static constexpr uint32_t KEPLER_CHANNEL_GROUP_A        = 0xA06C;
static constexpr size_t   CHANNEL_GROUP_PARAMS_BYTES    = 20;
static constexpr uint32_t AMPERE_CHANNEL_GPFIFO_A       = 0xC56F;
static constexpr size_t   CHANNEL_PARAMS_BYTES          = 360;
static constexpr uint32_t AMPERE_DMA_COPY_B             = 0xC7B5;
static constexpr size_t   COPY_PARAMS_BYTES             = 8;
static constexpr uint32_t GET_FAULT_METHOD_BUFFER_SIZE  = 0x20802A08;
static constexpr uint32_t BIND                          = 0xA06F0104;
static constexpr uint32_t GPFIFO_SCHEDULE               = 0xA06F0103;
static constexpr uint32_t ADDR_SYSMEM                   = 1;
static constexpr uint32_t ADDR_FBMEM                    = 2;
static constexpr uint32_t NV_MEMORY_CACHED              = 0;
static constexpr uint32_t NV_MEMORY_UNCACHED            = 1;
static constexpr uint32_t PRIVILEGE_USER                = 0x0;
static constexpr uint32_t PRIVILEGE_KERNEL              = 0x2;
static constexpr uint32_t USERD_GP_PUT                  = 0x8C;
static constexpr uint32_t VIRTUAL_FUNCTION_DOORBELL     = 0xBB0090;

// Byte offsets into NV_CHANNEL_ALLOC_PARAMS
static constexpr size_t GPFIFO_OFFSET_AT  = 8;
static constexpr size_t GPFIFO_ENTRIES_AT = 16;
static constexpr size_t FLAGS_AT          = 20;
static constexpr size_t CONTEXT_SHARE_AT  = 24;
static constexpr size_t VASPACE_AT        = 28;
static constexpr size_t ENGINE_TYPE_AT    = 128;
static constexpr size_t SUBDEVICE_ID_AT   = 136;
static constexpr size_t INSTANCE_MEM_AT   = 144;
static constexpr size_t USERD_MEM_AT      = 168;
static constexpr size_t RAMFC_MEM_AT      = 192;
static constexpr size_t METHOD_BUFFER_AT  = 216;
static constexpr size_t INTERNAL_FLAGS_AT = 244;

static constexpr gpu_objects TEST_OBJECTS        = {.device = 0xC1D00001, .subdevice = 0xC1D00002};
static constexpr rm::handle  SPACE_HANDLE        = 0xC1D00010;
static constexpr rm::handle  FIRST_HANDLE        = 0xC1D00020;
static constexpr uint64_t    FIRST_OFFSET        = 0x400000;
static constexpr uint32_t    FAULT_BUFFER_BYTES  = 20480;
static constexpr uint32_t    COPY2_ENGINE_TYPE   = 0xB;
static constexpr uint32_t    COPY2_RUNLIST       = 3;
static constexpr uint32_t    TEST_CHANNEL_ID     = 9;
static constexpr uint64_t    TEST_GPFIFO_ADDRESS = 0x100000;

// The pages the channel takes from system memory (USERD, the GPFIFO and two fault method buffers), and how far the free
// page count may move for other reasons during a test, counted with the quarantine of freed pages drained
static constexpr uint64_t CHANNEL_PAGES        = 1 + 32 + 5 + 5;
static constexpr uint64_t PAGE_COUNT_TOLERANCE = 8;

// A firmware recording each call's kind, objects, command or class, size and first 8 parameter words, plus the channel
// allocation's whole parameters, answering the fault method buffer's size and call `i` with `results[i]`
class gpfifo_channel_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 12;
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
    uint8_t    channel_params[CHANNEL_PARAMS_BYTES] = {};
    rm::handle next_handle = FIRST_HANDLE;

    int32_t allocate(rm::handle parent, rm::handle object, uint32_t class_id, void* params, size_t size) override {
        if (class_id == AMPERE_CHANNEL_GPFIFO_A && size == CHANNEL_PARAMS_BYTES) {
            string::memcpy(channel_params, params, size);
        }

        return record(kind::allocate, parent, object, class_id, params, size);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        if (command == GET_FAULT_METHOD_BUFFER_SIZE && size == sizeof(uint32_t)) {
            string::memcpy(params, &FAULT_BUFFER_BYTES, sizeof(uint32_t));
        }

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

    uint32_t channel_word(size_t offset) const {
        uint32_t word = 0;
        string::memcpy(&word, channel_params + offset, sizeof(word));
        return word;
    }

    uint64_t channel_doubleword(size_t offset) const {
        uint64_t doubleword = 0;
        string::memcpy(&doubleword, channel_params + offset, sizeof(doubleword));
        return doubleword;
    }

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

// The instance block's stand-in, which the CPU clears through `map_for_cpu`
alignas(4096) static uint8_t s_instance_block[4096];

// A video memory handing out ranges from `FIRST_OFFSET` on, mapping each for the CPU onto `s_instance_block`, recording
// releases and answering each page table update with `update_result`
class gpfifo_channel_video_memory : public video_memory {
public:
    static constexpr uint32_t MAX_RANGES = 16;

    uint64_t   next_offset = FIRST_OFFSET;
    vram_range released[MAX_RANGES] = {};
    uint32_t   releases = 0;
    uint32_t   unmaps_for_cpu = 0;
    int32_t    update_result = OK;

    int32_t allocate(uint64_t size, uint64_t, vram_use, vram_range& out) override {
        out = {.offset = next_offset, .size = size};
        next_offset += size;
        return OK;
    }

    void release(const vram_range& range) override {
        if (releases < MAX_RANGES) {
            released[releases] = range;
        }

        releases++;
    }

    int32_t map_for_cpu(const vram_range&, cpu_mapping& out) override {
        out = {.virt = reinterpret_cast<uintptr_t>(s_instance_block), .phys = 0};
        return OK;
    }

    void unmap_for_cpu(const vram_range&) override { unmaps_for_cpu++; }

    int32_t update_context_tables(uint64_t, table_writes&, context_tlbs) override { return update_result; }

    bool was_released(uint64_t offset) const {
        for (uint32_t i = 0; i < releases && i < MAX_RANGES; i++) {
            if (released[i].offset == offset) {
                return true;
            }
        }

        return false;
    }
};

// The doorbell's stand-in, which `registers` reaches at NVIDIA's offset from a base placed before it
static uint32_t s_doorbell = 0;

static registers doorbell_registers() {
    return registers(reinterpret_cast<uintptr_t>(&s_doorbell) - VIRTUAL_FUNCTION_DOORBELL);
}

static channel_setup copy2_setup(bool is_privileged) {
    return {
        .engine = {.type = COPY2_ENGINE_TYPE, .runlist = COPY2_RUNLIST},
        .id = TEST_CHANNEL_ID,
        .gpfifo_address = TEST_GPFIFO_ADDRESS,
        .is_privileged = is_privileged,
    };
}

static dma::device_profile test_card() {
    return dma::device_profile_test_access::build(64, true);
}

static chip_info channel_test_chip() {
    chip_info chip = {};
    chip.channel_class = AMPERE_CHANNEL_GPFIFO_A;
    chip.copy_class = AMPERE_DMA_COPY_B;
    return chip;
}

// The channel's objects and memory over doubles, creating the address space on the first `create`, with the instance
// block's stand-in filled with ones so its clearing shows
struct channel_fixture {
    gpfifo_channel_firmware     space_fw;
    gpfifo_channel_firmware     fw;
    gpfifo_channel_video_memory memory;
    failed_mark                 failed;
    address_space               space;
    channel                     queue;
    uint64_t                    instance_block_offset;

    channel_fixture()
        : space(space_fw, TEST_OBJECTS, memory, failed), queue(fw, TEST_OBJECTS, memory, doorbell_registers()) {
        space_fw.next_handle = SPACE_HANDLE;
        string::memset(s_instance_block, 0xFF, sizeof(s_instance_block));
        s_doorbell = 0;
        instance_block_offset = 0;
    }

    ~channel_fixture() {
        queue.destroy();
        space.destroy();
    }

    int32_t create(bool is_privileged) {
        if (space.handle() == 0 && space.create() != OK) {
            return ERR_BAD_STATE;
        }

        instance_block_offset = memory.next_offset;
        dma::device_profile card = test_card();
        return queue.create(card, channel_test_chip(), space, copy2_setup(is_privileged));
    }
};

// Create asks the subdevice for the fault method buffer's size, then allocates the group in the address space, the
// channel in the group with the memory the CPU side gives it, and the copy object, then binds and schedules it
TEST(nvidia_channel, create_allocates_the_group_the_channel_and_its_copy_object) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.create(true), OK);

    gpfifo_channel_firmware& fw = fixture.fw;
    ASSERT_EQ(fw.call_count, 6u);

    EXPECT_TRUE(fw.calls[0].what == gpfifo_channel_firmware::kind::control);
    EXPECT_EQ(fw.calls[0].object, TEST_OBJECTS.subdevice);
    EXPECT_EQ(fw.calls[0].number, GET_FAULT_METHOD_BUFFER_SIZE);

    const gpfifo_channel_firmware::call& group = fw.calls[1];
    EXPECT_TRUE(group.what == gpfifo_channel_firmware::kind::allocate);
    EXPECT_EQ(group.parent, TEST_OBJECTS.device);
    EXPECT_EQ(group.object, FIRST_HANDLE);
    EXPECT_EQ(group.number, KEPLER_CHANNEL_GROUP_A);
    EXPECT_EQ(group.size, CHANNEL_GROUP_PARAMS_BYTES);

    EXPECT_EQ(group.words[2], SPACE_HANDLE);
    EXPECT_EQ(group.words[3], COPY2_ENGINE_TYPE);

    const gpfifo_channel_firmware::call& allocation = fw.calls[2];
    EXPECT_TRUE(allocation.what == gpfifo_channel_firmware::kind::allocate);
    EXPECT_EQ(allocation.parent, FIRST_HANDLE);
    EXPECT_EQ(allocation.object, FIRST_HANDLE + 1);
    EXPECT_EQ(allocation.number, AMPERE_CHANNEL_GPFIFO_A);
    EXPECT_EQ(allocation.size, CHANNEL_PARAMS_BYTES);

    const gpfifo_channel_firmware::call& copy = fw.calls[3];
    EXPECT_TRUE(copy.what == gpfifo_channel_firmware::kind::allocate);
    EXPECT_EQ(copy.parent, FIRST_HANDLE + 1);
    EXPECT_EQ(copy.number, AMPERE_DMA_COPY_B);
    EXPECT_EQ(copy.size, COPY_PARAMS_BYTES);

    EXPECT_EQ(copy.words[0], 1u);
    EXPECT_EQ(copy.words[1], COPY2_ENGINE_TYPE);

    const gpfifo_channel_firmware::call& bind = fw.calls[4];
    EXPECT_TRUE(bind.what == gpfifo_channel_firmware::kind::control);
    EXPECT_EQ(bind.object, FIRST_HANDLE + 1);
    EXPECT_EQ(bind.number, BIND);
    EXPECT_EQ(bind.size, sizeof(uint32_t));
    EXPECT_EQ(bind.words[0], COPY2_ENGINE_TYPE);

    const gpfifo_channel_firmware::call& schedule = fw.calls[5];
    EXPECT_TRUE(schedule.what == gpfifo_channel_firmware::kind::control);
    EXPECT_EQ(schedule.object, FIRST_HANDLE + 1);
    EXPECT_EQ(schedule.number, GPFIFO_SCHEDULE);
    EXPECT_EQ(schedule.size, 2u);
    EXPECT_EQ(schedule.words[0] & 0xFFFF, 1u);

    EXPECT_EQ(fixture.queue.id(), TEST_CHANNEL_ID);
    EXPECT_EQ(fixture.queue.create(test_card(), channel_test_chip(), fixture.space, copy2_setup(true)), ERR_BAD_STATE);
}

// The kernel's channel names no address space or context share of its own, and takes the cleared instance block and
// RAMFC uncached in video memory and USERD and a fault method buffer cached in system memory
TEST(nvidia_channel, channel_takes_its_memory_from_the_cpu_side) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.create(true), OK);

    gpfifo_channel_firmware& fw = fixture.fw;
    EXPECT_EQ(fw.channel_doubleword(GPFIFO_OFFSET_AT), TEST_GPFIFO_ADDRESS);
    EXPECT_EQ(fw.channel_word(GPFIFO_ENTRIES_AT), GPFIFO_ENTRIES);
    EXPECT_EQ(fw.channel_word(FLAGS_AT), channel_allocation_flags(TEST_CHANNEL_ID, true));
    EXPECT_EQ(fw.channel_word(INTERNAL_FLAGS_AT), PRIVILEGE_KERNEL);

    EXPECT_EQ(fw.channel_word(CONTEXT_SHARE_AT), 0u);
    EXPECT_EQ(fw.channel_word(VASPACE_AT), 0u);
    EXPECT_EQ(fw.channel_word(ENGINE_TYPE_AT), COPY2_ENGINE_TYPE);
    EXPECT_EQ(fw.channel_word(SUBDEVICE_ID_AT), 0u);

    EXPECT_EQ(fw.channel_doubleword(INSTANCE_MEM_AT), fixture.instance_block_offset);
    EXPECT_EQ(fw.channel_doubleword(INSTANCE_MEM_AT + 8), 4096u);
    EXPECT_EQ(fw.channel_word(INSTANCE_MEM_AT + 16), ADDR_FBMEM);
    EXPECT_EQ(fw.channel_word(INSTANCE_MEM_AT + 20), NV_MEMORY_UNCACHED);

    EXPECT_EQ(fw.channel_doubleword(RAMFC_MEM_AT), fixture.instance_block_offset);
    EXPECT_EQ(fw.channel_doubleword(RAMFC_MEM_AT + 8), 512u);
    EXPECT_EQ(fw.channel_word(RAMFC_MEM_AT + 16), ADDR_FBMEM);
    EXPECT_EQ(fw.channel_word(RAMFC_MEM_AT + 20), NV_MEMORY_UNCACHED);

    EXPECT_NE(fw.channel_doubleword(USERD_MEM_AT), 0u);
    EXPECT_EQ(fw.channel_doubleword(USERD_MEM_AT + 8), 512u);
    EXPECT_EQ(fw.channel_word(USERD_MEM_AT + 16), ADDR_SYSMEM);
    EXPECT_EQ(fw.channel_word(USERD_MEM_AT + 20), NV_MEMORY_CACHED);

    EXPECT_NE(fw.channel_doubleword(METHOD_BUFFER_AT), 0u);
    EXPECT_EQ(fw.channel_doubleword(METHOD_BUFFER_AT + 8), FAULT_BUFFER_BYTES);
    EXPECT_EQ(fw.channel_word(METHOD_BUFFER_AT + 16), ADDR_SYSMEM);
    EXPECT_EQ(fw.channel_word(METHOD_BUFFER_AT + 20), NV_MEMORY_CACHED);

    bool is_cleared = true;
    for (uint8_t byte : s_instance_block) {
        is_cleared = is_cleared && byte == 0;
    }

    EXPECT_TRUE(is_cleared);
    EXPECT_EQ(fixture.memory.unmaps_for_cpu, 1u);
}

// A channel for a user's context runs with the user's privilege, and its flags allow no physical copies
TEST(nvidia_channel, user_channel_runs_with_user_privilege) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.create(false), OK);

    EXPECT_EQ(fixture.fw.channel_word(FLAGS_AT), channel_allocation_flags(TEST_CHANNEL_ID, false));
    EXPECT_EQ(fixture.fw.channel_word(INTERNAL_FLAGS_AT), PRIVILEGE_USER);
}

// An id from `CHANNEL_IDS` on is refused before anything is allocated
TEST(nvidia_channel, id_from_channel_ids_on_is_refused) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.space.create(), OK);
    uint64_t next_offset = fixture.memory.next_offset;

    channel_setup setup = copy2_setup(true);
    setup.id = drivers::nvidia::CHANNEL_IDS;
    EXPECT_EQ(fixture.queue.create(test_card(), channel_test_chip(), fixture.space, setup), ERR_INVALID_ARG);

    EXPECT_EQ(fixture.fw.call_count, 0u);
    EXPECT_EQ(fixture.memory.next_offset, next_offset);
}

// A refused channel releases its group and the memory, since the firmware then holds none of it
TEST(nvidia_channel, refused_channel_releases_the_group_and_the_memory) {
    page_quarantine::drain();
    uint64_t free_before = pmm::free_page_count();
    {
        channel_fixture fixture;
        fixture.fw.results[2] = ERR_FIRMWARE;
        EXPECT_EQ(fixture.create(true), ERR_FIRMWARE);

        ASSERT_EQ(fixture.fw.call_count, 4u);
        EXPECT_TRUE(fixture.fw.calls[3].what == gpfifo_channel_firmware::kind::release);
        EXPECT_EQ(fixture.fw.calls[3].object, FIRST_HANDLE);
        EXPECT_TRUE(fixture.memory.was_released(fixture.instance_block_offset));
        EXPECT_FALSE(fixture.queue.keeps_memory());
    }

    page_quarantine::drain();
    EXPECT_GE(pmm::free_page_count() + PAGE_COUNT_TOLERANCE, free_before);
}

// A refused release of the group keeps the memory for good, since the driver cannot tell what the firmware still holds
TEST(nvidia_channel, refused_group_release_keeps_the_memory) {
    page_quarantine::drain();
    uint64_t free_before = pmm::free_page_count();
    {
        channel_fixture fixture;
        fixture.fw.results[2] = ERR_FIRMWARE;
        fixture.fw.results[3] = ERR_FIRMWARE;
        EXPECT_EQ(fixture.create(true), ERR_FIRMWARE);

        EXPECT_FALSE(fixture.memory.was_released(fixture.instance_block_offset));
        EXPECT_TRUE(fixture.queue.keeps_memory());
    }

    page_quarantine::drain();
    EXPECT_LE(pmm::free_page_count() + CHANNEL_PAGES, free_before + PAGE_COUNT_TOLERANCE);
}

// A refused GPFIFO mapping allocates no objects and releases the memory
TEST(nvidia_channel, refused_mapping_releases_the_memory) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.space.create(), OK);
    fixture.memory.update_result = ERR_GPU_FAILED;

    EXPECT_EQ(fixture.create(true), ERR_GPU_FAILED);

    EXPECT_EQ(fixture.fw.call_count, 1u);
    EXPECT_TRUE(fixture.memory.was_released(fixture.instance_block_offset));
}

// Destroy releases the copy object, the channel and the group, the last allocated first, and the memory
TEST(nvidia_channel, destroy_releases_the_objects_last_first_and_the_memory) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.create(true), OK);

    EXPECT_EQ(fixture.queue.destroy(), OK);

    gpfifo_channel_firmware& fw = fixture.fw;
    ASSERT_EQ(fw.call_count, 9u);
    EXPECT_TRUE(fw.calls[6].what == gpfifo_channel_firmware::kind::release);
    EXPECT_EQ(fw.calls[6].object, FIRST_HANDLE + 2);
    EXPECT_EQ(fw.calls[7].object, FIRST_HANDLE + 1);
    EXPECT_EQ(fw.calls[8].object, FIRST_HANDLE);

    EXPECT_TRUE(fixture.memory.was_released(fixture.instance_block_offset));
    EXPECT_FALSE(fixture.queue.keeps_memory());
    EXPECT_EQ(fixture.queue.destroy(), ERR_BAD_STATE);
}

// A refused GPFIFO unmap keeps the memory for good, since the GPU may still translate to it
TEST(nvidia_channel, refused_unmap_keeps_the_memory) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.create(true), OK);
    fixture.memory.update_result = ERR_GPU_FAILED;

    EXPECT_EQ(fixture.queue.destroy(), ERR_GPU_FAILED);

    EXPECT_EQ(fixture.fw.call_count, 9u);
    EXPECT_FALSE(fixture.memory.was_released(fixture.instance_block_offset));
    EXPECT_TRUE(fixture.queue.keeps_memory());
    EXPECT_EQ(fixture.queue.destroy(), ERR_BAD_STATE);
}

// Entries land in consecutive slots, and the kick publishes GP_PUT past them and rings the doorbell with the runlist
// and the channel's id
TEST(nvidia_channel, kick_publishes_put_and_rings_the_doorbell) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.create(true), OK);

    fixture.queue.write_entry({.words = {0x11111111, 0x22222222}});
    fixture.queue.write_entry({.words = {0x33333333, 0x44444444}});
    EXPECT_EQ(s_doorbell, 0u);

    EXPECT_EQ(channel_test_access::gpfifo_word(fixture.queue, 0, 0), 0x11111111u);
    EXPECT_EQ(channel_test_access::gpfifo_word(fixture.queue, 0, 1), 0x22222222u);
    EXPECT_EQ(channel_test_access::gpfifo_word(fixture.queue, 1, 0), 0x33333333u);
    EXPECT_EQ(channel_test_access::gpfifo_word(fixture.queue, 1, 1), 0x44444444u);

    fixture.queue.kick();

    EXPECT_EQ(channel_test_access::userd_word(fixture.queue, USERD_GP_PUT), 2u);
    EXPECT_EQ(s_doorbell, (COPY2_RUNLIST << 16) | TEST_CHANNEL_ID);
}

// The slot after the ring's last is its first
TEST(nvidia_channel, ring_wraps_after_its_last_slot) {
    channel_fixture fixture;
    ASSERT_EQ(fixture.create(true), OK);
    channel_test_access::set_put(fixture.queue, GPFIFO_ENTRIES - 1);

    fixture.queue.write_entry({.words = {0x55555555, 0x66666666}});
    fixture.queue.kick();

    EXPECT_EQ(channel_test_access::gpfifo_word(fixture.queue, GPFIFO_ENTRIES - 1, 0), 0x55555555u);
    EXPECT_EQ(channel_test_access::userd_word(fixture.queue, USERD_GP_PUT), 0u);
}
