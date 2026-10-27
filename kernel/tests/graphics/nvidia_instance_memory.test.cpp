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

using drivers::nvidia::context_dma_memory;
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

// NVIDIA's hash table and records, restated for the same reason: 8-byte entries, then 32-byte records from 0x2020, each
// named in an entry by its offset in 32-byte units from bit 14, the first as 0x101
static constexpr uint32_t NVIDIA_HASH_ENTRIES        = 1024;
static constexpr uint64_t NVIDIA_ENTRY_BYTES         = 8;
static constexpr uint64_t NVIDIA_FIRST_RECORD_OFFSET = 0x2020;
static constexpr uint64_t NVIDIA_RECORD_BYTES        = 32;
static constexpr uint32_t NVIDIA_FIRST_RECORD_FIELD  = 0x101;
static constexpr uint32_t NVIDIA_RECORD_FIELD_SHIFT  = 14;

// An entry's client field and channel shift, and a record's first word for video memory and for snooped pages, each
// read and write and pitch
static constexpr uint32_t NVIDIA_CLIENT_MASK          = 0x3FFF;
static constexpr uint32_t NVIDIA_CHANNEL_SHIFT        = 25;
static constexpr uint32_t NVIDIA_VIDEO_MEMORY_WORD_0  = 5;
static constexpr uint32_t NVIDIA_SNOOPED_PAGES_WORD_0 = 7;

// NVIDIA's numbers for the core channel, window 0's channel and immediate channel, the last window's immediate channel
// and the first and last cursors' channels, and how many channel numbers there are, of which 65 to 72 name no channel
static constexpr uint32_t CORE_CHANNEL                  = 0;
static constexpr uint32_t WINDOW_CHANNEL                = 1;
static constexpr uint32_t WINDOW_IMMEDIATE_CHANNEL      = 33;
static constexpr uint32_t LAST_WINDOW_IMMEDIATE_CHANNEL = 64;
static constexpr uint32_t FIRST_CURSOR_CHANNEL          = 73;
static constexpr uint32_t LAST_CURSOR_CHANNEL           = 80;
static constexpr uint32_t NVIDIA_CHANNEL_NUMBERS        = 81;

// The driver's client (bits set in both bytes of its low 14 and in 15:14 past them, so an entry's client field too
// narrow or too wide is caught), context DMAs and the entries NVIDIA's hash gives their bindings under that client
static constexpr rm::handle DRIVER_CLIENT              = 0xC1D0'EA5F;
static constexpr rm::handle BUFFER_DMA                 = 0xC0DE'0001;
static constexpr rm::handle NOTIFIER_DMA               = 0xC0DE'0002;
static constexpr rm::handle SECOND_BUFFER_DMA          = 0xC0DE'0003;
static constexpr uint32_t   BUFFER_CORE_ENTRY          = 0x2D9;
static constexpr uint32_t   BUFFER_WINDOW_ENTRY        = 0x299;
static constexpr uint32_t   BUFFER_IMMEDIATE_ENTRY     = 0x29B;
static constexpr uint32_t   BUFFER_LAST_CURSOR_ENTRY   = 0x2DC;
static constexpr uint32_t   SECOND_BUFFER_WINDOW_ENTRY = 0x29B;

// A buffer in video memory, a notifier in pages above 1 TiB, so both of a record's high words are caught, and the last
// 256 bytes a record can name, with the five words of the record NVIDIA's layout gives each
static constexpr context_dma_memory BUFFER_MEMORY = {
    .in_video_memory = true,
    .address = 0x1'2340'0000,
    .bytes = 0xE1'0000,
};

static constexpr context_dma_memory NOTIFIER_MEMORY = {
    .in_video_memory = false,
    .address = 0x100'0000'1000,
    .bytes = 0x1000,
};

static constexpr context_dma_memory TOP_MEMORY = {
    .in_video_memory = true,
    .address = 0x7FFF'FFFF'FF00,
    .bytes = 0x100,
};

static constexpr uint32_t RECORD_WORDS      = 5;
static constexpr uint32_t BUFFER_RECORD[]   = {NVIDIA_VIDEO_MEMORY_WORD_0, 0x0123'4000, 0, 0x0124'20FF, 0};
static constexpr uint32_t NOTIFIER_RECORD[] = {NVIDIA_SNOOPED_PAGES_WORD_0, 0x10, 0x1, 0x1F, 0x1};
static constexpr uint32_t TOP_RECORD[]      = {NVIDIA_VIDEO_MEMORY_WORD_0, 0xFFFF'FFFF, 0x7F, 0xFFFF'FFFF, 0x7F};

// The nonzero words of the buffer's record and of an entry
static constexpr uint32_t BUFFER_RECORD_NONZERO_WORDS = 3;
static constexpr uint32_t ENTRY_NONZERO_WORDS         = 2;

// A binding's memory and the words of the record NVIDIA's layout gives it
struct record_case {
    context_dma_memory memory;
    const uint32_t*    words;
};

// A binding `bind` refuses for its handle, its channel or its memory
struct refused_binding {
    rm::handle         object;
    uint32_t           hardware_channel;
    context_dma_memory memory;
};

// A binding under `client` and the entry NVIDIA's hash gives it
struct hashed_binding {
    rm::handle client;
    rm::handle object;
    uint32_t   hardware_channel;
    uint32_t   entry;
};

// A range above 4 GiB, so a request carrying only the address's low word is caught, and the byte the memory holds
// before `set_up`, so a test sees whether it was cleared
static constexpr vram_range HANDED_OUT    = {.offset = 0x1'0007'0000, .size = NVIDIA_INSTANCE_MEMORY_BYTES};
static constexpr uint8_t    STALE_PATTERN = 0xA5;

// The memory the video memory double maps, in words so a mapping of it is aligned for every write, with a guard past
// the mapped bytes that a clear running past them would overwrite, and its copy at the last unmap
static constexpr uint64_t INSTANCE_MEMORY_WORDS = NVIDIA_INSTANCE_MEMORY_BYTES / sizeof(uint64_t);
static constexpr uint64_t GUARD_WORDS           = 512;
static uint64_t           g_backing[INSTANCE_MEMORY_WORDS + GUARD_WORDS];
static uint64_t           g_copy_at_unmap[INSTANCE_MEMORY_WORDS];

// The object under test, which its copy of the hash table makes too large for a test's stack
alignas(instance_memory) static uint8_t g_instance_bytes[sizeof(instance_memory)];

// A video memory that hands out `HANDED_OUT` and maps it over `g_backing`, recording each call, whether the memory was
// cleared at each unmap and, when asked, a copy of it, and refusing `allocate` or `map_for_cpu` with the result set
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
    bool       keeps_copy_at_unmap = false; // off where a test binds a thousand times, as each copy is 64 KiB

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
        if (keeps_copy_at_unmap) {
            string::memcpy(g_copy_at_unmap, g_backing, sizeof(g_copy_at_unmap));
        }
    }
};

// A firmware that answers internal controls with `result`, recording what each carried and how many unmaps `memory` had
// seen by then, names `driver_client` as the client, and counts the calls the instance memory never makes
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
    rm::handle                    driver_client = 0;

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

    int32_t control(rm::handle, uint32_t, void*, size_t) override {
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
        return driver_client;
    }
};

// An instance memory over `memory` and `fw`, built in `g_instance_bytes` as it is too large for a test's stack
static instance_memory& new_instance(recording_video_memory& memory, instance_memory_firmware& fw) {
    return *new (g_instance_bytes) instance_memory(memory, fw);
}

// An instance memory set up over `memory`, with `fw` naming `DRIVER_CLIENT` and `memory` copying itself at each unmap
static instance_memory& set_up_instance(recording_video_memory& memory, instance_memory_firmware& fw) {
    fw.driver_client = DRIVER_CLIENT;
    memory.keeps_copy_at_unmap = true;
    instance_memory& instance = new_instance(memory, fw);
    EXPECT_EQ(instance.set_up(), OK);

    return instance;
}

// The 32-bit word at `offset` in the memory's copy at the last unmap
static uint32_t word_at_unmap(uint64_t offset) {
    uint32_t word = 0;
    string::memcpy(&word, reinterpret_cast<const uint8_t*>(g_copy_at_unmap) + offset, sizeof(word));
    return word;
}

// Word `index` of hash table entry `entry` at the last unmap
static uint32_t entry_word(uint32_t entry, uint32_t index) {
    return word_at_unmap(entry * NVIDIA_ENTRY_BYTES + index * sizeof(uint32_t));
}

// Word `index` of record `record` at the last unmap
static uint32_t record_word(uint32_t record, uint32_t index) {
    return word_at_unmap(NVIDIA_FIRST_RECORD_OFFSET + record * NVIDIA_RECORD_BYTES + index * sizeof(uint32_t));
}

// Whether record `record` held `words` at the last unmap
static bool is_record_at_unmap(uint32_t record, const uint32_t* words) {
    for (uint32_t index = 0; index < RECORD_WORDS; index++) {
        if (record_word(record, index) != words[index]) {
            return false;
        }
    }

    return true;
}

// How many 32-bit words of the memory were not 0 at the last unmap
static uint32_t nonzero_words_at_unmap() {
    uint32_t nonzero = 0;
    for (uint64_t offset = 0; offset < NVIDIA_INSTANCE_MEMORY_BYTES; offset += sizeof(uint32_t)) {
        if (word_at_unmap(offset) != 0) {
            nonzero++;
        }
    }

    return nonzero;
}

// An entry's second word in NVIDIA's layout under `client`
static uint32_t nvidia_binding_word(uint32_t record, uint32_t hardware_channel, rm::handle client = DRIVER_CLIENT) {
    uint32_t client_bits = client & NVIDIA_CLIENT_MASK;
    uint32_t record_bits = (NVIDIA_FIRST_RECORD_FIELD + record) << NVIDIA_RECORD_FIELD_SHIFT;
    uint32_t channel_bits = hardware_channel << NVIDIA_CHANNEL_SHIFT;

    return client_bits | record_bits | channel_bits;
}

// The memory is NVIDIA's 64 KiB at NVIDIA's alignment, cleared and unmapped before its address goes out in one request
// in NVIDIA's layout as video memory mapped write-combined, after which the object holds the range
TEST(nvidia_instance_memory, set_up_clears_the_memory_and_sends_its_address_in_nvidias_layout) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = new_instance(memory, fw);
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
        instance_memory& instance = new_instance(memory, fw);

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
        instance_memory& instance = new_instance(memory, fw);

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
        instance_memory& instance = new_instance(memory, fw);

        int32_t rc = instance.set_up();

        EXPECT_EQ(rc, error);
        EXPECT_EQ(fw.control_count, 1u);
        EXPECT_EQ(memory.release_count, 0u);
        EXPECT_EQ(instance.range().offset, HANDED_OUT.offset);
        EXPECT_EQ(instance.range().size, HANDED_OUT.size);
    }
}

// A first binding writes its record into record 0 and the entry at NVIDIA's hash, through one mapping of the whole
// memory taken down after both, sending the firmware no request. The last cursor's channel sets the entry's bit 31
TEST(nvidia_instance_memory, bind_writes_the_record_and_the_entry_at_nvidias_hash_before_unmapping) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);

    int32_t rc = instance.bind(BUFFER_DMA, LAST_CURSOR_CHANNEL, BUFFER_MEMORY);

    EXPECT_EQ(rc, OK);
    EXPECT_EQ(memory.map_count, 2u);
    EXPECT_EQ(memory.mapped.offset, HANDED_OUT.offset);
    EXPECT_EQ(memory.mapped.size, HANDED_OUT.size);
    EXPECT_EQ(memory.unmap_count, 2u);
    EXPECT_EQ(memory.unmapped.offset, HANDED_OUT.offset);

    EXPECT_EQ(entry_word(BUFFER_LAST_CURSOR_ENTRY, 0), BUFFER_DMA);
    EXPECT_EQ(entry_word(BUFFER_LAST_CURSOR_ENTRY, 1), 0xA040'6A5Fu);
    EXPECT_TRUE(is_record_at_unmap(0, BUFFER_RECORD));
    EXPECT_EQ(nonzero_words_at_unmap(), ENTRY_NONZERO_WORDS + BUFFER_RECORD_NONZERO_WORDS);
    EXPECT_TRUE(memory.is_guard_intact());

    EXPECT_EQ(fw.control_count, 1u);
    EXPECT_EQ(fw.other_count, 0u);
}

// Memories whose records hold each target and a high word of 1 and of 0x7F, the most a record's 7 bits hold
static constexpr record_case RECORD_CASES[] = {
    {BUFFER_MEMORY, BUFFER_RECORD},
    {NOTIFIER_MEMORY, NOTIFIER_RECORD},
    {TOP_MEMORY, TOP_RECORD},
};

// A record names its memory's target, read and write access and pitch kind, then the memory's first and last 256-byte
// unit, each in a low and a high word
TEST(nvidia_instance_memory, a_record_names_its_memorys_target_and_first_and_last_256_byte_unit) {
    for (const record_case& expected : RECORD_CASES) {
        recording_video_memory memory;
        instance_memory_firmware fw(memory);
        instance_memory& instance = set_up_instance(memory, fw);

        int32_t rc = instance.bind(NOTIFIER_DMA, CORE_CHANNEL, expected.memory);

        EXPECT_EQ(rc, OK);
        EXPECT_TRUE(is_record_at_unmap(0, expected.words));
    }
}

// Context DMAs whose bindings to the core channel under `DRIVER_CLIENT` all hash to the last entry, and the entry their
// bindings to window 0's channel hash to
static constexpr rm::handle LAST_ENTRY_DMAS[]      = {0xC0DE'0127, 0xC0DE'0526, 0xC0DE'0925, 0xC0DE'0D24};
static constexpr uint32_t   LAST_ENTRY_WINDOW_ENTRY = 0x3BF;

// A binding takes the first free entry from its hash on, wrapping past the last entry to the first, and the lowest free
// record. Undoing the last entry's binding leaves those past it found, and a first binding hashing there takes it
TEST(nvidia_instance_memory, a_binding_takes_the_first_free_entry_from_its_hash_wrapping_past_the_last) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);
    uint32_t last_entry = NVIDIA_HASH_ENTRIES - 1;

    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[0], CORE_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[1], CORE_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[2], CORE_CHANNEL, BUFFER_MEMORY), OK);

    EXPECT_EQ(entry_word(last_entry, 0), LAST_ENTRY_DMAS[0]);
    EXPECT_EQ(entry_word(last_entry, 1), nvidia_binding_word(0, CORE_CHANNEL));
    EXPECT_EQ(entry_word(0, 0), LAST_ENTRY_DMAS[1]);
    EXPECT_EQ(entry_word(0, 1), nvidia_binding_word(1, CORE_CHANNEL));

    EXPECT_EQ(entry_word(1, 0), LAST_ENTRY_DMAS[2]);
    EXPECT_EQ(entry_word(1, 1), nvidia_binding_word(2, CORE_CHANNEL));
    EXPECT_EQ(entry_word(2, 0), 0u);

    EXPECT_EQ(instance.unbind(LAST_ENTRY_DMAS[0], CORE_CHANNEL), OK);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[2], CORE_CHANNEL, BUFFER_MEMORY), ERR_BAD_STATE);
    EXPECT_EQ(instance.unbind(LAST_ENTRY_DMAS[2], CORE_CHANNEL), OK);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[3], CORE_CHANNEL, NOTIFIER_MEMORY), OK);

    EXPECT_EQ(entry_word(last_entry, 0), LAST_ENTRY_DMAS[3]);
    EXPECT_EQ(entry_word(last_entry, 1), nvidia_binding_word(0, CORE_CHANNEL));
    EXPECT_TRUE(is_record_at_unmap(0, NOTIFIER_RECORD));
    EXPECT_EQ(entry_word(0, 0), LAST_ENTRY_DMAS[1]);
    EXPECT_EQ(entry_word(1, 0), 0u);
}

// The bindings in the last entry and the first are found at either end of the table: binding them again is refused as a
// repeat, and binding them to a window's channel shares their records
TEST(nvidia_instance_memory, a_repeat_or_further_binding_finds_the_bindings_in_the_first_and_last_entries) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[0], CORE_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[1], CORE_CHANNEL, BUFFER_MEMORY), OK);

    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[0], CORE_CHANNEL, BUFFER_MEMORY), ERR_BAD_STATE);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[1], CORE_CHANNEL, BUFFER_MEMORY), ERR_BAD_STATE);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[0], WINDOW_CHANNEL, NOTIFIER_MEMORY), OK);
    EXPECT_EQ(instance.bind(LAST_ENTRY_DMAS[1], WINDOW_CHANNEL, NOTIFIER_MEMORY), OK);

    EXPECT_EQ(entry_word(LAST_ENTRY_WINDOW_ENTRY, 1), nvidia_binding_word(0, WINDOW_CHANNEL));
    EXPECT_EQ(entry_word(LAST_ENTRY_WINDOW_ENTRY + 1, 1), nvidia_binding_word(1, WINDOW_CHANNEL));
    EXPECT_EQ(nonzero_words_at_unmap(), 4 * ENTRY_NONZERO_WORDS + 2 * BUFFER_RECORD_NONZERO_WORDS);
}

// The base of the handles that fill the hash table, and the entry NVIDIA's hash gives each of their bindings to the
// core channel under `DRIVER_CLIENT`, as handle j repeats j in its bits 9:0 and 19:10
static constexpr rm::handle FILLING_DMA_BASE = 0xD000'0000;
static constexpr uint32_t   FILLING_HASH     = 0x55;

// Filling handle j, whose binding after those of the j handles before it takes entry j past `FILLING_HASH`, wrapping
static rm::handle filling_dma(uint32_t j) {
    return FILLING_DMA_BASE | (j << 10) | j;
}

// Bindings hashing to one entry fill the table, the last searching all of it. Then another handle or channel is refused
// before mapping until an unbinding frees an entry, and a repeat as a repeat, as NVIDIA's driver seeks repeats first
TEST(nvidia_instance_memory, a_full_hash_table_refuses_a_binding_until_one_is_undone) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);
    memory.keeps_copy_at_unmap = false;
    uint32_t last_j = NVIDIA_HASH_ENTRIES - 1;

    uint32_t bound = 0;
    for (uint32_t j = 0; j < last_j; j++) {
        if (instance.bind(filling_dma(j), CORE_CHANNEL, BUFFER_MEMORY) == OK) {
            bound++;
        }
    }

    memory.keeps_copy_at_unmap = true;
    int32_t last_rc = instance.bind(filling_dma(last_j), CORE_CHANNEL, BUFFER_MEMORY);
    uint32_t maps_when_full = memory.map_count;
    int32_t rc = instance.bind(BUFFER_DMA, CORE_CHANNEL, BUFFER_MEMORY);
    int32_t further_rc = instance.bind(filling_dma(0), WINDOW_CHANNEL, BUFFER_MEMORY);
    int32_t repeat_rc = instance.bind(filling_dma(0), CORE_CHANNEL, BUFFER_MEMORY);

    EXPECT_EQ(bound, last_j);
    EXPECT_EQ(last_rc, OK);
    EXPECT_EQ(entry_word(FILLING_HASH - 1, 0), filling_dma(last_j));
    EXPECT_EQ(entry_word(FILLING_HASH - 1, 1), nvidia_binding_word(last_j, CORE_CHANNEL));

    EXPECT_EQ(rc, ERR_NO_MEMORY);
    EXPECT_EQ(further_rc, ERR_NO_MEMORY);
    EXPECT_EQ(repeat_rc, ERR_BAD_STATE);
    EXPECT_EQ(memory.map_count, maps_when_full);
    EXPECT_TRUE(memory.is_guard_intact());

    EXPECT_EQ(instance.unbind(filling_dma(0), CORE_CHANNEL), OK);
    EXPECT_EQ(instance.bind(BUFFER_DMA, CORE_CHANNEL, NOTIFIER_MEMORY), OK);

    EXPECT_EQ(entry_word(FILLING_HASH, 0), BUFFER_DMA);
    EXPECT_EQ(entry_word(FILLING_HASH, 1), nvidia_binding_word(0, CORE_CHANNEL));
    EXPECT_TRUE(is_record_at_unmap(0, NOTIFIER_RECORD));
}

// A second binding of a context DMA to the same channel is refused before anything is mapped
TEST(nvidia_instance_memory, a_second_binding_of_a_context_dma_to_a_channel_is_refused) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), OK);

    int32_t rc = instance.bind(BUFFER_DMA, WINDOW_CHANNEL, NOTIFIER_MEMORY);

    EXPECT_EQ(rc, ERR_BAD_STATE);
    EXPECT_EQ(memory.map_count, 2u);
    EXPECT_EQ(nonzero_words_at_unmap(), ENTRY_NONZERO_WORDS + BUFFER_RECORD_NONZERO_WORDS);
}

// Memory starting 128 bytes past a 256-byte boundary
static constexpr context_dma_memory UNALIGNED_MEMORY = {
    .in_video_memory = true,
    .address = 0x1'2340'0080,
    .bytes = 0x100,
};

// Bindings of a context DMA share the record its first binding wrote, which keeps naming that binding's memory, and a
// binding whose hash's entry is taken takes the next entry. A further binding's memory is checked as a first one's
TEST(nvidia_instance_memory, bindings_of_a_context_dma_share_its_record) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);

    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_IMMEDIATE_CHANNEL, NOTIFIER_MEMORY), OK);
    EXPECT_EQ(instance.bind(SECOND_BUFFER_DMA, WINDOW_CHANNEL, NOTIFIER_MEMORY), OK);

    EXPECT_EQ(entry_word(BUFFER_WINDOW_ENTRY, 1), nvidia_binding_word(0, WINDOW_CHANNEL));
    EXPECT_EQ(entry_word(BUFFER_IMMEDIATE_ENTRY, 1), nvidia_binding_word(0, WINDOW_IMMEDIATE_CHANNEL));
    EXPECT_EQ(entry_word(SECOND_BUFFER_WINDOW_ENTRY + 1, 0), SECOND_BUFFER_DMA);
    EXPECT_EQ(entry_word(SECOND_BUFFER_WINDOW_ENTRY + 1, 1), nvidia_binding_word(1, WINDOW_CHANNEL));
    EXPECT_TRUE(is_record_at_unmap(0, BUFFER_RECORD));
    EXPECT_TRUE(is_record_at_unmap(1, NOTIFIER_RECORD));

    uint32_t maps_before = memory.map_count;
    EXPECT_EQ(instance.bind(BUFFER_DMA, CORE_CHANNEL, UNALIGNED_MEMORY), ERR_INVALID_ARG);
    EXPECT_EQ(memory.map_count, maps_before);
    EXPECT_EQ(instance.bind(BUFFER_DMA, CORE_CHANNEL, BUFFER_MEMORY), OK);
}

// A record shared by a context DMA's bindings outlives each unbinding but the last, so a first binding meanwhile takes
// another record. Once both are free, the next first binding takes the lower, though it was freed later
TEST(nvidia_instance_memory, a_shared_record_is_freed_at_its_last_unbinding) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(instance.bind(BUFFER_DMA, CORE_CHANNEL, BUFFER_MEMORY), OK);

    EXPECT_EQ(instance.unbind(BUFFER_DMA, WINDOW_CHANNEL), OK);
    EXPECT_EQ(instance.bind(NOTIFIER_DMA, CORE_CHANNEL, NOTIFIER_MEMORY), OK);

    EXPECT_EQ(entry_word(BUFFER_WINDOW_ENTRY, 0), 0u);
    EXPECT_EQ(entry_word(BUFFER_WINDOW_ENTRY, 1), 0u);
    EXPECT_TRUE(is_record_at_unmap(0, BUFFER_RECORD));
    EXPECT_TRUE(is_record_at_unmap(1, NOTIFIER_RECORD));

    EXPECT_EQ(instance.unbind(NOTIFIER_DMA, CORE_CHANNEL), OK);
    EXPECT_EQ(instance.unbind(BUFFER_DMA, CORE_CHANNEL), OK);
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, TOP_MEMORY), OK);

    EXPECT_EQ(entry_word(BUFFER_WINDOW_ENTRY, 1), nvidia_binding_word(0, WINDOW_CHANNEL));
    EXPECT_TRUE(is_record_at_unmap(0, TOP_RECORD));
    EXPECT_TRUE(is_record_at_unmap(1, NOTIFIER_RECORD));
}

// Undoing a context DMA's only binding clears its entry to zeros through one mapping of the whole memory, taken down
// after the write, and leaves its freed record's words, sending the firmware no request. Undoing it again is refused
TEST(nvidia_instance_memory, unbind_clears_the_entry_and_leaves_the_freed_records_words) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);
    EXPECT_EQ(instance.bind(BUFFER_DMA, LAST_CURSOR_CHANNEL, BUFFER_MEMORY), OK);

    int32_t rc = instance.unbind(BUFFER_DMA, LAST_CURSOR_CHANNEL);

    EXPECT_EQ(rc, OK);
    EXPECT_EQ(memory.map_count, 3u);
    EXPECT_EQ(memory.mapped.offset, HANDED_OUT.offset);
    EXPECT_EQ(memory.mapped.size, HANDED_OUT.size);

    EXPECT_EQ(memory.unmap_count, 3u);
    EXPECT_EQ(memory.unmapped.offset, HANDED_OUT.offset);
    EXPECT_EQ(memory.unmapped.size, HANDED_OUT.size);

    EXPECT_EQ(entry_word(BUFFER_LAST_CURSOR_ENTRY, 0), 0u);
    EXPECT_EQ(entry_word(BUFFER_LAST_CURSOR_ENTRY, 1), 0u);
    EXPECT_TRUE(is_record_at_unmap(0, BUFFER_RECORD));
    EXPECT_EQ(nonzero_words_at_unmap(), BUFFER_RECORD_NONZERO_WORDS);
    EXPECT_TRUE(memory.is_guard_intact());

    EXPECT_EQ(fw.control_count, 1u);
    EXPECT_EQ(fw.other_count, 0u);
    EXPECT_EQ(instance.unbind(BUFFER_DMA, LAST_CURSOR_CHANNEL), ERR_BAD_STATE);
    EXPECT_EQ(memory.map_count, 3u);
}

// Undoing a binding the object does not hold is refused before anything is mapped: before `set_up`, for the handle 0
// every free entry holds, for another context DMA or channel and for a number whose low byte names the bound channel
TEST(nvidia_instance_memory, unbinding_what_is_not_bound_is_refused) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    EXPECT_EQ(new_instance(memory, fw).unbind(BUFFER_DMA, CORE_CHANNEL), ERR_BAD_STATE);
    EXPECT_EQ(memory.map_count, 0u);

    instance_memory& instance = set_up_instance(memory, fw);
    EXPECT_EQ(instance.bind(BUFFER_DMA, CORE_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(instance.unbind(0, CORE_CHANNEL), ERR_BAD_STATE);
    EXPECT_EQ(instance.unbind(NOTIFIER_DMA, CORE_CHANNEL), ERR_BAD_STATE);
    EXPECT_EQ(instance.unbind(BUFFER_DMA, WINDOW_CHANNEL), ERR_BAD_STATE);
    EXPECT_EQ(instance.unbind(BUFFER_DMA, 0x100 + CORE_CHANNEL), ERR_BAD_STATE);

    EXPECT_EQ(memory.map_count, 2u);
    EXPECT_EQ(instance.unbind(BUFFER_DMA, CORE_CHANNEL), OK);
}

// Bindings refused for handle 0, the channel numbers 81, 65 and 72 or one whose low byte names a channel, or memory
// unaligned, empty, ending a byte past 128 TiB, starting there, or wrapping past 2^64 from inside 128 TiB and past it
static constexpr refused_binding REFUSED_BINDINGS[] = {
    {0, CORE_CHANNEL, BUFFER_MEMORY},
    {BUFFER_DMA, NVIDIA_CHANNEL_NUMBERS, BUFFER_MEMORY},
    {BUFFER_DMA, LAST_WINDOW_IMMEDIATE_CHANNEL + 1, BUFFER_MEMORY},
    {BUFFER_DMA, FIRST_CURSOR_CHANNEL - 1, BUFFER_MEMORY},
    {BUFFER_DMA, 0x100 + WINDOW_CHANNEL, BUFFER_MEMORY},
    {BUFFER_DMA, CORE_CHANNEL, UNALIGNED_MEMORY},
    {BUFFER_DMA, CORE_CHANNEL, {.in_video_memory = true, .address = 0x1'2340'0000, .bytes = 0}},
    {BUFFER_DMA, CORE_CHANNEL, {.in_video_memory = true, .address = 0x7FFF'FFFF'FF00, .bytes = 0x101}},
    {BUFFER_DMA, CORE_CHANNEL, {.in_video_memory = false, .address = 0x8000'0000'0000, .bytes = 0x100}},
    {BUFFER_DMA, CORE_CHANNEL, {.in_video_memory = true, .address = 0x1000, .bytes = 0xFFFF'FFFF'FFFF'F100}},
    {BUFFER_DMA, CORE_CHANNEL, {.in_video_memory = false, .address = 0xFFFF'FFFF'FFFF'FF00, .bytes = 0x200}},
};

// Each is refused before anything is mapped and takes nothing, so a valid binding after them takes the first record.
// Each is refused again once `BUFFER_DMA` is bound, so a guard a further binding skips is caught
TEST(nvidia_instance_memory, a_binding_of_handle_0_no_channel_or_memory_no_record_can_name_is_refused) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    instance_memory& instance = set_up_instance(memory, fw);
    for (const refused_binding& refused : REFUSED_BINDINGS) {
        EXPECT_EQ(instance.bind(refused.object, refused.hardware_channel, refused.memory), ERR_INVALID_ARG);
    }

    EXPECT_EQ(memory.map_count, 1u);
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(nonzero_words_at_unmap(), ENTRY_NONZERO_WORDS + BUFFER_RECORD_NONZERO_WORDS);
    EXPECT_TRUE(is_record_at_unmap(0, BUFFER_RECORD));

    for (const refused_binding& refused : REFUSED_BINDINGS) {
        EXPECT_EQ(instance.bind(refused.object, refused.hardware_channel, refused.memory), ERR_INVALID_ARG);
    }

    EXPECT_EQ(memory.map_count, 2u);
}

// Handles, then clients, with one bit set: the lowest, the top bit of a hash term (handle bits 9 and 29, client bit 7)
// or the highest. Then the channel numbers 64 and 73 on each side of 65 to 72, whose bits 6 and 3 top their terms
static constexpr hashed_binding HASHED_BINDINGS[] = {
    {DRIVER_CLIENT, 0x0000'0001, CORE_CHANNEL, 0x157},
    {DRIVER_CLIENT, 0x0000'0200, CORE_CHANNEL, 0x356},
    {DRIVER_CLIENT, 0x2000'0000, CORE_CHANNEL, 0x356},
    {DRIVER_CLIENT, 0x8000'0000, CORE_CHANNEL, 0x154},
    {0x0000'0001, BUFFER_DMA, CORE_CHANNEL, 0x38B},
    {0x0000'0080, BUFFER_DMA, CORE_CHANNEL, 0x18F},
    {0x8000'0000, BUFFER_DMA, CORE_CHANNEL, 0x38F},
    {DRIVER_CLIENT, BUFFER_DMA, LAST_WINDOW_IMMEDIATE_CHANNEL, 0x2DD},
    {DRIVER_CLIENT, BUFFER_DMA, FIRST_CURSOR_CHANNEL, 0x09D},
};

// Each binding takes the entry NVIDIA's hash gives it in an empty table, so a hash dropping a term's top bit is caught
TEST(nvidia_instance_memory, bindings_with_one_bit_handles_or_clients_or_channels_64_and_73_are_at_nvidias_hash) {
    for (const hashed_binding& expected : HASHED_BINDINGS) {
        recording_video_memory memory;
        instance_memory_firmware fw(memory);
        instance_memory& instance = set_up_instance(memory, fw);
        fw.driver_client = expected.client;

        int32_t rc = instance.bind(expected.object, expected.hardware_channel, BUFFER_MEMORY);

        EXPECT_EQ(rc, OK);
        EXPECT_EQ(entry_word(expected.entry, 0), expected.object);
        EXPECT_EQ(entry_word(expected.entry, 1), nvidia_binding_word(0, expected.hardware_channel, expected.client));
    }
}

// A binding before `set_up` has cleared the memory, or a first or further one while the driver has no client, is
// refused before anything is mapped and takes nothing
TEST(nvidia_instance_memory, a_binding_without_the_memory_set_up_or_a_client_is_refused) {
    recording_video_memory memory;
    instance_memory_firmware fw(memory);
    fw.driver_client = DRIVER_CLIENT;
    instance_memory& instance = new_instance(memory, fw);
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), ERR_BAD_STATE);
    EXPECT_EQ(memory.map_count, 0u);

    fw.driver_client = 0;
    memory.keeps_copy_at_unmap = true;
    EXPECT_EQ(instance.set_up(), OK);
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), ERR_BAD_STATE);
    EXPECT_EQ(memory.map_count, 1u);

    fw.driver_client = DRIVER_CLIENT;
    EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(entry_word(BUFFER_WINDOW_ENTRY, 1), nvidia_binding_word(0, WINDOW_CHANNEL));

    fw.driver_client = 0;
    EXPECT_EQ(instance.bind(BUFFER_DMA, CORE_CHANNEL, BUFFER_MEMORY), ERR_BAD_STATE);
    EXPECT_EQ(memory.map_count, 2u);

    fw.driver_client = DRIVER_CLIENT;
    EXPECT_EQ(instance.bind(BUFFER_DMA, CORE_CHANNEL, BUFFER_MEMORY), OK);
    EXPECT_EQ(entry_word(BUFFER_CORE_ENTRY, 1), nvidia_binding_word(0, CORE_CHANNEL));
}

// A failed mapping makes `bind` take neither an entry nor a record and `unbind` keep the binding and its record, after
// every error `video_memory::map_for_cpu` returns
TEST(nvidia_instance_memory, a_failed_mapping_leaves_the_bindings_as_they_were) {
    for (int32_t error : MAP_ERRORS) {
        recording_video_memory memory;
        instance_memory_firmware fw(memory);
        instance_memory& instance = set_up_instance(memory, fw);
        EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), OK);

        memory.map_result = error;
        EXPECT_EQ(instance.bind(SECOND_BUFFER_DMA, WINDOW_CHANNEL, NOTIFIER_MEMORY), error);
        EXPECT_EQ(instance.unbind(BUFFER_DMA, WINDOW_CHANNEL), error);
        EXPECT_EQ(memory.unmap_count, 2u);

        memory.map_result = OK;
        EXPECT_EQ(instance.bind(BUFFER_DMA, WINDOW_CHANNEL, BUFFER_MEMORY), ERR_BAD_STATE);
        EXPECT_EQ(instance.bind(SECOND_BUFFER_DMA, WINDOW_CHANNEL, NOTIFIER_MEMORY), OK);
        EXPECT_EQ(entry_word(SECOND_BUFFER_WINDOW_ENTRY, 1), nvidia_binding_word(1, WINDOW_CHANNEL));
    }
}
