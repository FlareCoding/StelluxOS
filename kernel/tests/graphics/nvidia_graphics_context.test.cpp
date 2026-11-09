// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/graphics_context.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_graphics_context);

using drivers::nvidia::channel_context_buffers;
using drivers::nvidia::context_buffer;
using drivers::nvidia::context_buffer_layout;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::firmware;
using drivers::nvidia::global_context_buffers;
using drivers::nvidia::graphics_buffer_layouts;
using drivers::nvidia::OK;
using drivers::nvidia::place_global_buffers;
using drivers::nvidia::place_own_buffers;
using drivers::nvidia::promote_graphics_context;
using drivers::nvidia::read_graphics_buffer_layouts;
namespace rm = drivers::nvidia::rm;

// NVIDIA's two internal controls, their replies' sizes and rows, and the positions of the facts and buffers read,
// restated so the tests hold the reads to NVIDIA's layout rather than to the driver's
static constexpr uint32_t KGR_GET_INFO                 = 0x20800A2A;
static constexpr size_t   KGR_GET_INFO_BYTES           = 3392;
static constexpr uint32_t INFO_ROW_FACTS               = 0x35;
static constexpr uint32_t MAX_SUBCONTEXT_COUNT         = 0x2C;
static constexpr uint32_t KGR_GET_CONTEXT_BUFFERS_INFO = 0x20800A32;
static constexpr size_t   CONTEXT_BUFFERS_BYTES        = 1600;
static constexpr uint32_t BUFFERS_ROW_ENTRIES          = 0x19;
static constexpr uint32_t ENGINE_ID_GRAPHICS           = 0x00;
static constexpr uint32_t ENGINE_ID_PATCH              = 0x10;
static constexpr uint32_t ENGINE_ID_BUNDLE_CB          = 0x11;
static constexpr uint32_t ENGINE_ID_PAGEPOOL_GLOBAL    = 0x12;
static constexpr uint32_t ENGINE_ID_ATTRIBUTE_CB       = 0x13;
static constexpr uint32_t ENGINE_ID_RTV_CB_GLOBAL      = 0x14;
static constexpr uint32_t ENGINE_ID_FECS_EVENT         = 0x17;
static constexpr uint32_t ENGINE_ID_PRIV_ACCESS_MAP    = 0x18;
static constexpr uint32_t NV_U32_MAX                   = 0xFFFFFFFF;

// NVIDIA's PROMOTE_CTX: the control, its parameters' size and the offsets of the words the driver sets, an entry's
// offsets, the buffer ids, and the attributes of uncached video memory
static constexpr uint32_t GPU_PROMOTE_CTX          = 0x2080012B;
static constexpr size_t   PROMOTE_CTX_BYTES        = 560;
static constexpr size_t   ENGINE_TYPE_OFFSET       = 0;
static constexpr size_t   CHAN_CLIENT_OFFSET       = 12;
static constexpr size_t   OBJECT_OFFSET            = 16;
static constexpr size_t   ENTRY_COUNT_OFFSET       = 40;
static constexpr size_t   FIRST_ENTRY_OFFSET       = 48;
static constexpr size_t   ENTRY_BYTES              = 32;
static constexpr size_t   ENTRY_VIRT_OFFSET        = 8;
static constexpr size_t   ENTRY_SIZE_OFFSET        = 16;
static constexpr size_t   ENTRY_ATTR_OFFSET        = 24;
static constexpr size_t   ENTRY_ID_OFFSET          = 28;
static constexpr size_t   ENTRY_INITIALIZE_OFFSET  = 30;
static constexpr size_t   ENTRY_NONMAPPED_OFFSET   = 31;
static constexpr uint32_t ENGINE_TYPE_GR0          = 1;
static constexpr uint16_t ID_MAIN                  = 0;
static constexpr uint16_t ID_PATCH                 = 2;
static constexpr uint16_t ID_BUNDLE_CB             = 3;
static constexpr uint16_t ID_PAGEPOOL              = 4;
static constexpr uint16_t ID_ATTRIBUTE_CB          = 5;
static constexpr uint16_t ID_RTV_CB_GLOBAL         = 6;
static constexpr uint16_t ID_FECS_EVENT            = 9;
static constexpr uint16_t ID_PRIV_ACCESS_MAP       = 10;
static constexpr uint16_t ID_UNRESTRICTED_MAP      = 11;
static constexpr uint32_t VIDMEM_GPU_UNCACHED_ATTR = 0x4;

static constexpr rm::handle TEST_CLIENT    = 0xC1D00001;
static constexpr rm::handle TEST_SUBDEVICE = 0x00050002;
static constexpr rm::handle TEST_CHANNEL   = 0x00050011;

// GA102's sizes and subcontext count as the RTX 3080's firmware reports them for GR0, its main buffer grown by 64
// headers of 4 KiB after rounding up to 4 KiB
static constexpr uint32_t RTX_3080_SUBCONTEXTS           = 64;
static constexpr uint32_t RTX_3080_MAIN_BYTES            = 0x129B00;
static constexpr uint64_t RTX_3080_MAIN_WITH_HEADERS     = 0x16A000;
static constexpr uint32_t RTX_3080_PATCH_BYTES           = 0x5000;
static constexpr uint32_t RTX_3080_BUNDLE_BYTES          = 0x3000;
static constexpr uint32_t RTX_3080_PAGE_POOL_BYTES       = 0x20000;
static constexpr uint32_t RTX_3080_ATTRIBUTE_BYTES       = 0x851200;
static constexpr uint32_t RTX_3080_RTV_BYTES             = 0x80000;
static constexpr uint32_t RTX_3080_FECS_EVENT_BYTES      = 0x10000;
static constexpr uint32_t RTX_3080_PRIV_ACCESS_MAP_BYTES = 0x80000;
static constexpr uint32_t PAGE_ALIGNMENT                 = 0x1000;
static constexpr uint32_t SMALL_ALIGNMENT                = 0x100;

// What the second engine's rows hold, which a read of the wrong row would return
static constexpr uint32_t OTHER_ROW_WORD = 0x7777;

// The value a layout the read must leave alone starts with
static constexpr uint64_t STALE_BYTES = 0xA5A5A5A5;

// Answers the two internal controls with GR0's facts and sizes in NVIDIA's layout, recording each call, and keeps the
// bytes of the last PROMOTE_CTX
class graphics_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 4;

    struct call {
        uint32_t command;
        size_t   size;
    };

    uint32_t subcontext_fact_index = MAX_SUBCONTEXT_COUNT;
    uint32_t subcontexts = RTX_3080_SUBCONTEXTS;
    uint32_t buffer_sizes[BUFFERS_ROW_ENTRIES] = {};
    uint32_t buffer_alignments[BUFFERS_ROW_ENTRIES] = {};
    int32_t info_result = OK;
    int32_t buffers_result = OK;
    int32_t promote_result = OK;
    call calls[MAX_CALLS] = {};
    uint32_t call_count = 0;

    uint32_t   promotions = 0;
    rm::handle promoted_on = 0;
    uint8_t    promoted[PROMOTE_CTX_BYTES] = {};

    graphics_firmware() {
        set_buffer(ENGINE_ID_GRAPHICS, RTX_3080_MAIN_BYTES, PAGE_ALIGNMENT);
        set_buffer(ENGINE_ID_PATCH, RTX_3080_PATCH_BYTES, PAGE_ALIGNMENT);

        set_buffer(ENGINE_ID_BUNDLE_CB, RTX_3080_BUNDLE_BYTES, SMALL_ALIGNMENT);
        set_buffer(ENGINE_ID_PAGEPOOL_GLOBAL, RTX_3080_PAGE_POOL_BYTES, SMALL_ALIGNMENT);
        set_buffer(ENGINE_ID_ATTRIBUTE_CB, RTX_3080_ATTRIBUTE_BYTES, PAGE_ALIGNMENT);
        set_buffer(ENGINE_ID_RTV_CB_GLOBAL, RTX_3080_RTV_BYTES, PAGE_ALIGNMENT);
        set_buffer(ENGINE_ID_FECS_EVENT, RTX_3080_FECS_EVENT_BYTES, PAGE_ALIGNMENT);
        set_buffer(ENGINE_ID_PRIV_ACCESS_MAP, RTX_3080_PRIV_ACCESS_MAP_BYTES, PAGE_ALIGNMENT);
    }

    void set_buffer(uint32_t position, uint32_t size, uint32_t alignment) {
        buffer_sizes[position] = size;
        buffer_alignments[position] = alignment;
    }

    int32_t internal_control(uint32_t command, void* params, size_t size) override {
        if (call_count < MAX_CALLS) {
            calls[call_count] = {.command = command, .size = size};
        }

        call_count++;

        auto* words = static_cast<uint32_t*>(params);
        if (command == KGR_GET_INFO && size == KGR_GET_INFO_BYTES) {
            fill_facts(words);
            return info_result;
        }

        if (command == KGR_GET_CONTEXT_BUFFERS_INFO && size == CONTEXT_BUFFERS_BYTES) {
            fill_buffers(words);
            return buffers_result;
        }

        return ERR_FIRMWARE;
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        if (command != GPU_PROMOTE_CTX || size != PROMOTE_CTX_BYTES) {
            return ERR_FIRMWARE;
        }

        promotions++;
        promoted_on = object;
        string::memcpy(promoted, params, size);
        return promote_result;
    }

    int32_t control_with_status(rm::handle object, uint32_t command, void* params, size_t size, uint32_t&) override {
        return control(object, command, params, size);
    }

    // No function under test sends these, so a stray one shows as a failed call
    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override { return ERR_FIRMWARE; }

    int32_t release(rm::handle) override { return ERR_FIRMWARE; }

    rm::handle new_handle() override { return 0; }

    rm::handle client() const override { return TEST_CLIENT; }

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

    void set_error_sink(drivers::nvidia::firmware_error_sink*) override {}

private:
    // Each fact of GR0's row at the position its index names, and the second engine's row of other words
    void fill_facts(uint32_t* words) {
        for (uint32_t i = 0; i < INFO_ROW_FACTS; i++) {
            words[i * 2] = i;
            words[(INFO_ROW_FACTS + i) * 2 + 1] = OTHER_ROW_WORD;
        }

        words[MAX_SUBCONTEXT_COUNT * 2] = subcontext_fact_index;
        words[MAX_SUBCONTEXT_COUNT * 2 + 1] = subcontexts;
    }

    void fill_buffers(uint32_t* words) {
        for (uint32_t i = 0; i < BUFFERS_ROW_ENTRIES; i++) {
            words[i * 2] = buffer_sizes[i];
            words[i * 2 + 1] = buffer_alignments[i];
            words[(BUFFERS_ROW_ENTRIES + i) * 2] = OTHER_ROW_WORD;
            words[(BUFFERS_ROW_ENTRIES + i) * 2 + 1] = OTHER_ROW_WORD;
        }
    }
};

// The promoted parameters' value of type `T` at `offset`
template <typename T>
static T promoted_field(const graphics_firmware& fw, size_t offset) {
    T value = 0;
    string::memcpy(&value, fw.promoted + offset, sizeof(value));
    return value;
}

// Whether promoted entry `index` names `buffer` as `id`, its video memory for the firmware to initialize when
// `initializes` and its address when `is_mapped`, as NVIDIA reads an entry
static bool is_entry(
    const graphics_firmware& fw,
    uint32_t index,
    uint16_t id,
    const context_buffer& buffer,
    bool initializes,
    bool is_mapped
) {
    size_t at = FIRST_ENTRY_OFFSET + index * ENTRY_BYTES;

    uint64_t phys = initializes ? buffer.range.offset : 0;
    uint64_t size = initializes ? buffer.range.size : 0;
    uint32_t attributes = initializes ? VIDMEM_GPU_UNCACHED_ATTR : 0;
    uint8_t initialize_flag = initializes ? 1 : 0;

    uint64_t virt = is_mapped ? buffer.gpu_address : 0;
    uint8_t nonmapped_flag = initializes && !is_mapped ? 1 : 0;

    bool is_id = promoted_field<uint16_t>(fw, at + ENTRY_ID_OFFSET) == id;
    bool is_placed = promoted_field<uint64_t>(fw, at) == phys
                  && promoted_field<uint64_t>(fw, at + ENTRY_SIZE_OFFSET) == size;
    bool is_initialization = promoted_field<uint32_t>(fw, at + ENTRY_ATTR_OFFSET) == attributes
                          && promoted_field<uint8_t>(fw, at + ENTRY_INITIALIZE_OFFSET) == initialize_flag;
    bool is_mapping = promoted_field<uint64_t>(fw, at + ENTRY_VIRT_OFFSET) == virt
                   && promoted_field<uint8_t>(fw, at + ENTRY_NONMAPPED_OFFSET) == nonmapped_flag;

    return is_id && is_placed && is_initialization && is_mapping;
}

// Gives every buffer a distinct video memory offset, size and address, the unrestricted map no address
static void make_distinct(global_context_buffers& global, channel_context_buffers& own) {
    context_buffer* all[] = {
        &own.main,
        &own.patch,
        &global.bundle,
        &global.page_pool,
        &global.attribute,
        &global.rtv,
        &global.fecs_event,
        &global.priv_access_map,
        &global.unrestricted_priv_access_map,
    };

    for (uint64_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        all[i]->range = {.offset = 0x1000000 + i * 0x100000, .size = 0x2000 + i * 0x1000};
        all[i]->gpu_address = 0x8040000000 + i * 0x100000;
    }

    global.unrestricted_priv_access_map.gpu_address = 0;
}

static graphics_buffer_layouts stale_layouts() {
    graphics_buffer_layouts layouts = {};
    layouts.main = {.bytes = STALE_BYTES, .alignment = STALE_BYTES};
    return layouts;
}

static bool is_unchanged(const graphics_buffer_layouts& layouts) {
    return layouts.main.bytes == STALE_BYTES && layouts.main.alignment == STALE_BYTES;
}

static bool equals(const context_buffer_layout& layout, uint64_t bytes, uint64_t alignment) {
    return layout.bytes == bytes && layout.alignment == alignment;
}

// The RTX 3080's reply gives GR0's layouts, asked for with NVIDIA's controls and sizes, the main buffer grown by a
// header per subcontext
TEST(nvidia_graphics_context, rtx_3080_reply_gives_gr0s_layouts) {
    graphics_firmware fw;
    graphics_buffer_layouts layouts = stale_layouts();

    ASSERT_EQ(read_graphics_buffer_layouts(fw, layouts), OK);

    ASSERT_EQ(fw.call_count, 2u);
    EXPECT_EQ(fw.calls[0].command, KGR_GET_INFO);
    EXPECT_EQ(fw.calls[0].size, KGR_GET_INFO_BYTES);
    EXPECT_EQ(fw.calls[1].command, KGR_GET_CONTEXT_BUFFERS_INFO);
    EXPECT_EQ(fw.calls[1].size, CONTEXT_BUFFERS_BYTES);

    EXPECT_TRUE(equals(layouts.main, RTX_3080_MAIN_WITH_HEADERS, PAGE_ALIGNMENT));
    EXPECT_TRUE(equals(layouts.patch, RTX_3080_PATCH_BYTES, PAGE_ALIGNMENT));

    EXPECT_TRUE(equals(layouts.bundle, RTX_3080_BUNDLE_BYTES, SMALL_ALIGNMENT));
    EXPECT_TRUE(equals(layouts.page_pool, RTX_3080_PAGE_POOL_BYTES, SMALL_ALIGNMENT));
    EXPECT_TRUE(equals(layouts.attribute, RTX_3080_ATTRIBUTE_BYTES, PAGE_ALIGNMENT));
    EXPECT_TRUE(equals(layouts.rtv, RTX_3080_RTV_BYTES, PAGE_ALIGNMENT));
    EXPECT_TRUE(equals(layouts.fecs_event, RTX_3080_FECS_EVENT_BYTES, PAGE_ALIGNMENT));
    EXPECT_TRUE(equals(layouts.priv_access_map, RTX_3080_PRIV_ACCESS_MAP_BYTES, PAGE_ALIGNMENT));
}

// A main buffer already a multiple of 4 KiB grows by the headers alone
TEST(nvidia_graphics_context, aligned_main_buffer_grows_by_the_headers_alone) {
    graphics_firmware fw;
    fw.set_buffer(ENGINE_ID_GRAPHICS, 0x2000, PAGE_ALIGNMENT);
    fw.subcontexts = 2;
    graphics_buffer_layouts layouts = stale_layouts();

    ASSERT_EQ(read_graphics_buffer_layouts(fw, layouts), OK);

    EXPECT_EQ(layouts.main.bytes, 0x4000u);
}

// A subcontext count whose headers pass 4 GiB grows the main buffer by every header
TEST(nvidia_graphics_context, main_buffer_grows_by_headers_past_4_gib) {
    graphics_firmware fw;
    fw.subcontexts = 0x80000000;
    graphics_buffer_layouts layouts = stale_layouts();

    ASSERT_EQ(read_graphics_buffer_layouts(fw, layouts), OK);

    EXPECT_EQ(layouts.main.bytes, 0x12A000 + 0x80000000ULL * 0x1000);
}

// A buffer the driver allocates sized 0 or as missing is refused, leaving the layouts unchanged
TEST(nvidia_graphics_context, unsized_buffer_is_refused) {
    graphics_firmware fw;
    fw.set_buffer(ENGINE_ID_PATCH, 0, PAGE_ALIGNMENT);
    graphics_buffer_layouts layouts = stale_layouts();

    EXPECT_EQ(read_graphics_buffer_layouts(fw, layouts), ERR_BAD_MESSAGE);
    EXPECT_TRUE(is_unchanged(layouts));

    graphics_firmware missing_fw;
    missing_fw.set_buffer(ENGINE_ID_PRIV_ACCESS_MAP, NV_U32_MAX, PAGE_ALIGNMENT);

    EXPECT_EQ(read_graphics_buffer_layouts(missing_fw, layouts), ERR_BAD_MESSAGE);
    EXPECT_TRUE(is_unchanged(layouts));
}

// Sizes up to one short of the size marking a missing buffer are kept as the firmware gives them
TEST(nvidia_graphics_context, sizes_short_of_missing_are_kept) {
    graphics_firmware fw;
    fw.set_buffer(ENGINE_ID_BUNDLE_CB, NV_U32_MAX - 1, SMALL_ALIGNMENT);
    fw.set_buffer(ENGINE_ID_FECS_EVENT, 0x7FFFFFFF, PAGE_ALIGNMENT);
    graphics_buffer_layouts layouts = stale_layouts();

    ASSERT_EQ(read_graphics_buffer_layouts(fw, layouts), OK);

    EXPECT_TRUE(equals(layouts.bundle, NV_U32_MAX - 1, SMALL_ALIGNMENT));
    EXPECT_TRUE(equals(layouts.fecs_event, 0x7FFFFFFF, PAGE_ALIGNMENT));
}

// A subcontext count out of its place or of 0 is refused before the sizes are asked for
TEST(nvidia_graphics_context, subcontext_count_out_of_place_or_zero_is_refused) {
    const uint32_t misplaced_indexes[] = {
        MAX_SUBCONTEXT_COUNT - 1,
        MAX_SUBCONTEXT_COUNT + 1,
        MAX_SUBCONTEXT_COUNT + 0x10000,
    };

    graphics_buffer_layouts layouts = stale_layouts();

    for (uint32_t index : misplaced_indexes) {
        graphics_firmware fw;
        fw.subcontext_fact_index = index;

        EXPECT_EQ(read_graphics_buffer_layouts(fw, layouts), ERR_BAD_MESSAGE);
        EXPECT_TRUE(is_unchanged(layouts));
        EXPECT_EQ(fw.call_count, 1u);
    }

    graphics_firmware zero_fw;
    zero_fw.subcontexts = 0;

    EXPECT_EQ(read_graphics_buffer_layouts(zero_fw, layouts), ERR_BAD_MESSAGE);
    EXPECT_TRUE(is_unchanged(layouts));
}

// Either control's error comes back as it is, the first one's before the second control is sent
TEST(nvidia_graphics_context, controls_errors_come_back_as_they_are) {
    graphics_firmware fw;
    fw.info_result = ERR_FIRMWARE;
    graphics_buffer_layouts layouts = stale_layouts();

    EXPECT_EQ(read_graphics_buffer_layouts(fw, layouts), ERR_FIRMWARE);
    EXPECT_EQ(fw.call_count, 1u);
    EXPECT_TRUE(is_unchanged(layouts));

    graphics_firmware buffers_fw;
    buffers_fw.buffers_result = ERR_FIRMWARE;

    EXPECT_EQ(read_graphics_buffer_layouts(buffers_fw, layouts), ERR_FIRMWARE);
    EXPECT_EQ(buffers_fw.call_count, 2u);
    EXPECT_TRUE(is_unchanged(layouts));
}

// An address the unrestricted map holds before placement, which placement clears
static constexpr uint64_t STALE_GPU_ADDRESS = 0x8040800000;

// The attribute buffer goes first, at a multiple of its size rounded up to a power of two, and each other global
// buffer follows at a multiple of 4 KiB, leaving the unrestricted map without an address
TEST(nvidia_graphics_context, rtx_3080_global_buffers_follow_the_attribute_buffer) {
    graphics_firmware fw;
    graphics_buffer_layouts layouts = {};
    ASSERT_EQ(read_graphics_buffer_layouts(fw, layouts), OK);

    global_context_buffers global = {};
    channel_context_buffers own = {};
    make_distinct(global, own);
    global.unrestricted_priv_access_map.gpu_address = STALE_GPU_ADDRESS;
    uint64_t end = place_global_buffers(layouts, (1ULL << 39) + 0x1000, global);

    EXPECT_EQ(global.attribute.gpu_address, (1ULL << 39) + 0x1000000);
    EXPECT_EQ(global.bundle.gpu_address, global.attribute.gpu_address + 0x852000);
    EXPECT_EQ(global.page_pool.gpu_address, global.bundle.gpu_address + RTX_3080_BUNDLE_BYTES);
    EXPECT_EQ(global.rtv.gpu_address, global.page_pool.gpu_address + RTX_3080_PAGE_POOL_BYTES);
    EXPECT_EQ(global.fecs_event.gpu_address, global.rtv.gpu_address + RTX_3080_RTV_BYTES);
    EXPECT_EQ(global.priv_access_map.gpu_address, global.fecs_event.gpu_address + RTX_3080_FECS_EVENT_BYTES);

    EXPECT_EQ(end, global.priv_access_map.gpu_address + RTX_3080_PRIV_ACCESS_MAP_BYTES);
    EXPECT_EQ(global.unrestricted_priv_access_map.gpu_address, 0u);
}

// Each queue slot's main buffer follows the slot before it by both buffers' bytes, and its patch buffer follows it
TEST(nvidia_graphics_context, rtx_3080_slots_own_buffers_follow_one_another) {
    graphics_firmware fw;
    graphics_buffer_layouts layouts = {};
    ASSERT_EQ(read_graphics_buffer_layouts(fw, layouts), OK);

    channel_context_buffers first = {};
    channel_context_buffers third = {};
    place_own_buffers(layouts, (1ULL << 39) + 0x1000, 0, first);
    place_own_buffers(layouts, (1ULL << 39) + 0x1000, 2, third);

    uint64_t slot_bytes = RTX_3080_MAIN_WITH_HEADERS + RTX_3080_PATCH_BYTES;
    EXPECT_EQ(first.main.gpu_address, (1ULL << 39) + 0x1000);
    EXPECT_EQ(first.patch.gpu_address, first.main.gpu_address + RTX_3080_MAIN_WITH_HEADERS);
    EXPECT_EQ(third.main.gpu_address, first.main.gpu_address + 2 * slot_bytes);
    EXPECT_EQ(third.patch.gpu_address, third.main.gpu_address + RTX_3080_MAIN_WITH_HEADERS);
}

// A patch buffer aligned past 4 KiB starts at its alignment's next multiple, and each slot's span is a multiple of that
// alignment, so the next slot's patch buffer stays aligned
TEST(nvidia_graphics_context, own_buffer_aligned_past_a_page_stays_aligned_in_every_slot) {
    graphics_firmware fw;
    fw.set_buffer(ENGINE_ID_PATCH, RTX_3080_PATCH_BYTES, 0x100000);
    graphics_buffer_layouts layouts = {};
    ASSERT_EQ(read_graphics_buffer_layouts(fw, layouts), OK);

    channel_context_buffers first = {};
    channel_context_buffers second = {};
    place_own_buffers(layouts, (1ULL << 39) + 0x1000, 0, first);
    place_own_buffers(layouts, (1ULL << 39) + 0x1000, 1, second);

    EXPECT_EQ(first.main.gpu_address, (1ULL << 39) + 0x100000);
    EXPECT_EQ(first.patch.gpu_address, first.main.gpu_address + 0x200000);
    EXPECT_EQ(second.main.gpu_address, first.main.gpu_address + 0x300000);
    EXPECT_EQ(second.patch.gpu_address, second.main.gpu_address + 0x200000);
}

// The golden context's promotion names the channel to the subdevice, initializes the channel's own buffers, the FECS
// event buffer and both register access maps, and maps every buffer but the unrestricted map
TEST(nvidia_graphics_context, golden_promotion_initializes_what_the_firmware_fills) {
    graphics_firmware fw;
    global_context_buffers global = {};
    channel_context_buffers own = {};
    make_distinct(global, own);

    ASSERT_EQ(promote_graphics_context(fw, TEST_SUBDEVICE, TEST_CHANNEL, global, own, true), OK);

    ASSERT_EQ(fw.promotions, 1u);
    EXPECT_EQ(fw.promoted_on, TEST_SUBDEVICE);
    EXPECT_EQ(promoted_field<uint32_t>(fw, ENGINE_TYPE_OFFSET), ENGINE_TYPE_GR0);
    EXPECT_EQ(promoted_field<uint32_t>(fw, CHAN_CLIENT_OFFSET), TEST_CLIENT);
    EXPECT_EQ(promoted_field<uint32_t>(fw, OBJECT_OFFSET), TEST_CHANNEL);
    ASSERT_EQ(promoted_field<uint32_t>(fw, ENTRY_COUNT_OFFSET), 9u);

    EXPECT_TRUE(is_entry(fw, 0, ID_MAIN, own.main, true, true));
    EXPECT_TRUE(is_entry(fw, 1, ID_PATCH, own.patch, true, true));

    EXPECT_TRUE(is_entry(fw, 2, ID_BUNDLE_CB, global.bundle, false, true));
    EXPECT_TRUE(is_entry(fw, 3, ID_PAGEPOOL, global.page_pool, false, true));
    EXPECT_TRUE(is_entry(fw, 4, ID_ATTRIBUTE_CB, global.attribute, false, true));
    EXPECT_TRUE(is_entry(fw, 5, ID_RTV_CB_GLOBAL, global.rtv, false, true));

    EXPECT_TRUE(is_entry(fw, 6, ID_FECS_EVENT, global.fecs_event, true, true));
    EXPECT_TRUE(is_entry(fw, 7, ID_PRIV_ACCESS_MAP, global.priv_access_map, true, true));
    EXPECT_TRUE(is_entry(fw, 8, ID_UNRESTRICTED_MAP, global.unrestricted_priv_access_map, true, false));
}

// A later channel's promotion initializes only its own buffers and maps the global ones as the golden context left them
TEST(nvidia_graphics_context, channel_promotion_only_maps_the_global_buffers) {
    graphics_firmware fw;
    global_context_buffers global = {};
    channel_context_buffers own = {};
    make_distinct(global, own);

    ASSERT_EQ(promote_graphics_context(fw, TEST_SUBDEVICE, TEST_CHANNEL, global, own, false), OK);

    ASSERT_EQ(promoted_field<uint32_t>(fw, ENTRY_COUNT_OFFSET), 8u);
    EXPECT_TRUE(is_entry(fw, 0, ID_MAIN, own.main, true, true));
    EXPECT_TRUE(is_entry(fw, 1, ID_PATCH, own.patch, true, true));
    EXPECT_TRUE(is_entry(fw, 6, ID_FECS_EVENT, global.fecs_event, false, true));
    EXPECT_TRUE(is_entry(fw, 7, ID_PRIV_ACCESS_MAP, global.priv_access_map, false, true));
}

// The firmware's refusal of a promotion comes back as it is
TEST(nvidia_graphics_context, promotion_error_comes_back_as_it_is) {
    graphics_firmware fw;
    fw.promote_result = ERR_FIRMWARE;
    global_context_buffers global = {};
    channel_context_buffers own = {};
    make_distinct(global, own);

    EXPECT_EQ(promote_graphics_context(fw, TEST_SUBDEVICE, TEST_CHANNEL, global, own, true), ERR_FIRMWARE);
    EXPECT_EQ(fw.promotions, 1u);
}
