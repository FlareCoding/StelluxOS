// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/core_methods.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/display_engine.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/rm.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/vram_allocator.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_display_engine);

using drivers::nvidia::CHANNEL_GET;
using drivers::nvidia::CHANNEL_PUT;
using drivers::nvidia::chip_info;
using drivers::nvidia::CORE_CHANNEL_AREA;
using drivers::nvidia::CORE_INITIAL_METHODS_PER_HEAD;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::display_engine;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::INSTANCE_MEMORY_BYTES;
using drivers::nvidia::OK;
using drivers::nvidia::registers;
using drivers::nvidia::release_core_channel;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
namespace rm = drivers::nvidia::rm;

// The driver's client and device and the first handle the double gives, made up
static constexpr rm::handle TEST_CLIENT  = 0xC1D0000B;
static constexpr rm::handle TEST_DEVICE  = 0x0005000A;
static constexpr rm::handle FIRST_HANDLE = 0x0005000D;

// A display class and a core channel class no chip has, so each allocation's class names its object
static constexpr uint32_t TEST_DISPLAY_CLASS      = 0xC67F;
static constexpr uint32_t TEST_CORE_CHANNEL_CLASS = 0xC67E;

// A card's full address width, and a limit `dma::alloc_pages` refuses, so a set-up for a card limited to it fails at
// the push buffer
static constexpr uint8_t FULL_ADDRESS_BITS    = 64;
static constexpr uint8_t REFUSED_ADDRESS_BITS = 16;

// The heads of the chip `set_up` describes, whose initial methods outgrow the push buffer
static constexpr uint8_t CHIP_HEADS = 4;

// The heads the double reports by default, where their initial methods (each a header and one value) end, so a GET
// reading it has reached PUT, and a GET past the push buffer's writable end
static constexpr uint8_t  DOUBLE_HEADS        = 2;
static constexpr uint32_t INITIAL_METHODS_END = CORE_INITIAL_METHODS_PER_HEAD * DOUBLE_HEADS * 2 * sizeof(uint32_t);
static constexpr uint32_t BAD_GET             = 0xFFFFFFFF;

// The memory the video memory double maps, and the words standing in for the core channel's PUT and GET
static uint8_t  g_video_memory[INSTANCE_MEMORY_BYTES];
static uint32_t g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t) + 1];

// The object under test, which its instance memory's copy of the hash table makes too large for a test's stack
alignas(display_engine) static uint8_t g_engine_bytes[sizeof(display_engine)];

// A firmware that records the kind, object and number of each request in order, answers a request a refusal matches
// with its result, the head reads with `heads` heads, and others with their parameters as sent
class engine_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 32;

    enum class kind { allocation, control, release, internal_control };

    struct call {
        kind       made;
        rm::handle object; // the allocated, controlled or released object, 0 for an internal control
        uint32_t   number; // the class of an allocation or the command of a control, else 0
    };

    // Matches the calls of its kind whose number and object equal its own, each 0 matching any
    struct refusal {
        kind       made;
        uint32_t   number;
        rm::handle object;
        int32_t    result;
    };

    call     calls[MAX_CALLS] = {};
    uint32_t call_count = 0;
    uint32_t handle_count = 0;
    refusal  refusals[2] = {};
    uint8_t  heads = DOUBLE_HEADS;

    int32_t allocate(rm::handle, rm::handle object, uint32_t class_id, void*, size_t) override {
        return answer(kind::allocation, object, class_id);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t) override {
        int32_t rc = answer(kind::control, object, command);
        if (rc == OK && params != nullptr) {
            fill_head_reply(command, params);
        }

        return rc;
    }

    int32_t release(rm::handle object) override {
        return answer(kind::release, object, 0);
    }

    int32_t internal_control(uint32_t command, void*, size_t) override {
        return answer(kind::internal_control, 0, command);
    }

    rm::handle new_handle() override {
        return FIRST_HANDLE + handle_count++;
    }

    rm::handle client() const override {
        return TEST_CLIENT;
    }

    // The handle of the allocation of `class_id`, or 0 when none was made
    rm::handle allocated(uint32_t class_id) const {
        for (uint32_t i = 0; i < recorded(); i++) {
            if (calls[i].made == kind::allocation && calls[i].number == class_id) {
                return calls[i].object;
            }
        }

        return 0;
    }

    // How many of the calls so far release an object
    uint32_t release_count() const {
        uint32_t releases = 0;
        for (uint32_t i = 0; i < recorded(); i++) {
            if (calls[i].made == kind::release) {
                releases++;
            }
        }

        return releases;
    }

    // Whether the calls from `first` on release `object` exactly once
    bool releases_once(uint32_t first, rm::handle object) const {
        uint32_t releases = 0;
        for (uint32_t i = first; i < recorded(); i++) {
            if (calls[i].made == kind::release && calls[i].object == object) {
                releases++;
            }
        }

        return releases == 1;
    }

    // Whether the calls from `first` on begin with every call `other` recorded, in the same order
    bool begins_with(uint32_t first, const engine_firmware& other) const {
        if (first + other.recorded() > recorded()) {
            return false;
        }

        for (uint32_t i = 0; i < other.recorded(); i++) {
            const call& mine = calls[first + i];
            const call& theirs = other.calls[i];

            bool is_same_kind = mine.made == theirs.made;
            bool is_same_object = mine.object == theirs.object;
            bool is_same_number = mine.number == theirs.number;
            if (!is_same_kind || !is_same_object || !is_same_number) {
                return false;
            }
        }

        return true;
    }

private:
    // How many calls `calls` holds
    uint32_t recorded() const {
        return call_count < MAX_CALLS ? call_count : MAX_CALLS;
    }

    // Records the call and gives the result of the first refusal matching it, else `OK`
    int32_t answer(kind made, rm::handle object, uint32_t number) {
        if (call_count < MAX_CALLS) {
            calls[call_count] = {.made = made, .object = object, .number = number};
        }

        call_count++;

        for (const refusal& refused : refusals) {
            bool is_number = refused.number == 0 || refused.number == number;
            bool is_object = refused.object == 0 || refused.object == object;
            if (refused.result != OK && refused.made == made && is_number && is_object) {
                return refused.result;
            }
        }

        return OK;
    }

    // Gives `heads` heads the display engine can use, from head 0, each served by the window of its number
    void fill_head_reply(uint32_t command, void* params) const {
        if (command == rm::CONTROL_GET_HEAD_COUNT) {
            static_cast<rm::head_count_params*>(params)->head_count = heads;
        } else if (command == rm::CONTROL_GET_HEAD_MASK) {
            static_cast<rm::head_mask_params*>(params)->head_mask = (1u << heads) - 1;
        } else if (command == rm::CONTROL_GET_WINDOW_HEADS) {
            rm::window_heads_params* reply = static_cast<rm::window_heads_params*>(params);
            for (uint8_t head = 0; head < heads; head++) {
                reply->window_heads[head] = static_cast<uint8_t>(1u << head);
            }
        }
    }
};

// A video memory that hands out offset 0, refusing more bytes than `g_video_memory` holds, and maps it over that array,
// so a release or an unmap has nothing to undo
class engine_video_memory : public video_memory {
public:
    int32_t allocate(uint64_t size, uint64_t, vram_use, vram_range& out) override {
        if (size > sizeof(g_video_memory)) {
            return ERR_NO_MEMORY;
        }

        out = {.offset = 0, .size = size};
        return OK;
    }

    void release(const vram_range&) override {}

    int32_t map_for_cpu(const vram_range&, cpu_mapping& out) override {
        out.virt = reinterpret_cast<uintptr_t>(g_video_memory);
        out.phys = 0;
        return OK;
    }

    void unmap_for_cpu(const vram_range&) override {}
};

// An engine over `fw` and `memory`, built in `g_engine_bytes`
static display_engine& new_engine(engine_firmware& fw, engine_video_memory& memory) {
    return *new (g_engine_bytes) display_engine(fw, memory);
}

// Sets up `engine` under `TEST_DEVICE` for a card driving `address_bits` physical address bits, with four heads and
// eight windows. PUT and GET are in `g_core_channel_registers`, GET reading `get`, and no push buffer page is freed
static int32_t set_up(
    display_engine& engine,
    uint8_t address_bits = FULL_ADDRESS_BITS,
    uint32_t get = INITIAL_METHODS_END
) {
    g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t)] = get;
    registers regs(reinterpret_cast<uintptr_t>(g_core_channel_registers) - CORE_CHANNEL_AREA);
    dma::device_profile card = dma::device_profile_test_access::build(address_bits, true);

    chip_info chip = {};
    chip.display = {
        .head_count         = CHIP_HEADS,
        .window_count       = 8,
        .display_class      = TEST_DISPLAY_CLASS,
        .core_channel_class = TEST_CORE_CHANNEL_CLASS,
        .displayport        = {},
    };

    return engine.set_up(regs, card, chip, TEST_DEVICE);
}

// An engine that has set up nothing sends no request on its release, which returns `OK`
TEST(nvidia_display_engine, release_before_set_up_sends_nothing) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.call_count, 0u);
}

// After a full set-up the release first sends what `release_core_channel` sends for the channel and the display it was
// allocated under, then releases each of the display's objects once, and a second release sends nothing
TEST(nvidia_display_engine, release_frees_the_core_channel_before_the_objects_once) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    rm::handle display = fw.allocated(TEST_DISPLAY_CLASS);
    rm::handle bandwidth_check = fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK);
    rm::handle display_common = fw.allocated(rm::CLASS_DISPLAY_COMMON);
    rm::handle channel = fw.allocated(TEST_CORE_CHANNEL_CLASS);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    engine_firmware channel_release;
    EXPECT_EQ(release_core_channel(channel_release, display, channel), OK);

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.begins_with(first, channel_release));

    EXPECT_TRUE(fw.releases_once(first, channel));
    EXPECT_TRUE(fw.releases_once(first, display));
    EXPECT_TRUE(fw.releases_once(first, bandwidth_check));
    EXPECT_TRUE(fw.releases_once(first, display_common));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.call_count, first + 5);
}

// A refused release of the core channel still releases the objects, and the release returns the channel's error rather
// than a refused object release's
TEST(nvidia_display_engine, refused_core_channel_release_still_releases_the_objects) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    rm::handle channel = fw.allocated(TEST_CORE_CHANNEL_CLASS);
    rm::handle display_common = fw.allocated(rm::CLASS_DISPLAY_COMMON);
    uint32_t first = fw.call_count;

    fw.refusals[0] = {.made = engine_firmware::kind::release, .number = 0, .object = channel, .result = ERR_TIMEOUT};
    fw.refusals[1] = {
        .made   = engine_firmware::kind::release,
        .number = 0,
        .object = display_common,
        .result = ERR_FIRMWARE,
    };

    EXPECT_EQ(engine.release(), ERR_TIMEOUT);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, channel));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, display_common));
}

// When the core channel's release succeeds, the release returns a refused object release's error
TEST(nvidia_display_engine, object_release_error_is_returned_when_the_channel_release_succeeds) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    rm::handle channel = fw.allocated(TEST_CORE_CHANNEL_CLASS);
    uint32_t first = fw.call_count;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::release,
        .number = 0,
        .object = fw.allocated(rm::CLASS_DISPLAY_COMMON),
        .result = ERR_FIRMWARE,
    };

    EXPECT_EQ(engine.release(), ERR_FIRMWARE);
    EXPECT_TRUE(fw.releases_once(first, channel));
}

// A core channel whose allocation failed is released only after an error that may leave the firmware holding it, and
// the objects are released either way
TEST(nvidia_display_engine, failed_core_channel_is_released_only_when_the_firmware_may_hold_it) {
    struct failure {
        int32_t error;
        bool    is_channel_released;
    };

    const failure failures[] = {{ERR_TIMEOUT, true}, {ERR_FIRMWARE, false}};
    for (const failure& failed : failures) {
        engine_firmware fw;
        engine_video_memory memory;
        display_engine& engine = new_engine(fw, memory);
        fw.refusals[0] = {
            .made   = engine_firmware::kind::allocation,
            .number = TEST_CORE_CHANNEL_CLASS,
            .object = 0,
            .result = failed.error,
        };

        EXPECT_EQ(set_up(engine), failed.error);
        EXPECT_EQ(fw.release_count(), 0u);
        uint32_t first = fw.call_count;

        EXPECT_EQ(engine.release(), OK);
        ASSERT_EQ(fw.call_count, first + (failed.is_channel_released ? 5 : 3));
        EXPECT_EQ(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)), failed.is_channel_released);
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
    }
}

// A set-up refused at the display's capabilities keeps the objects it allocated until the release frees them, with no
// request for a core channel it never allocated
TEST(nvidia_display_engine, set_up_failing_at_the_capabilities_keeps_the_objects_until_release) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_GET_DISPLAY_CAPABILITIES,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    EXPECT_EQ(set_up(engine), ERR_FIRMWARE);
    EXPECT_EQ(fw.allocated(TEST_CORE_CHANNEL_CLASS), 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 3);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A set-up whose push buffer cannot be allocated fails with `ERR_NO_MEMORY` and keeps the objects until the release
// frees them, with no request for a core channel
TEST(nvidia_display_engine, set_up_failing_at_the_push_buffer_keeps_the_objects_until_release) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up(engine, REFUSED_ADDRESS_BITS), ERR_NO_MEMORY);
    EXPECT_EQ(fw.allocated(TEST_CORE_CHANNEL_CLASS), 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 3);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A set-up kicks the core channel's initial methods for the double's two heads, its PUT left where they end
TEST(nvidia_display_engine, set_up_kicks_the_initial_methods_of_the_heads_read) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    ASSERT_EQ(set_up(engine), OK);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
}

// A GET past the push buffer's writable end fails the set-up with `ERR_BAD_STATE` once the initial methods are kicked,
// and the core channel and the objects stay allocated until the release frees them
TEST(nvidia_display_engine, set_up_failing_at_the_initial_methods_keeps_the_core_channel_until_release) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    EXPECT_EQ(set_up(engine, FULL_ADDRESS_BITS, BAD_GET), ERR_BAD_STATE);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// The initial methods of four heads outgrow the push buffer, so a GET past its writable end fails the set-up with
// `ERR_BAD_STATE` in a write, before any kick, and the core channel and the objects stay allocated until the release
TEST(nvidia_display_engine, set_up_failing_in_a_write_kicks_nothing_and_keeps_the_core_channel) {
    engine_firmware fw;
    fw.heads = CHIP_HEADS;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    EXPECT_EQ(set_up(engine, FULL_ADDRESS_BITS, BAD_GET), ERR_BAD_STATE);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}
