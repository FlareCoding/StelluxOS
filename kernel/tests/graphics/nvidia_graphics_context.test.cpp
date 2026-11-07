// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/graphics_context.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_graphics_context);

using drivers::nvidia::context_buffer_layout;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::firmware;
using drivers::nvidia::graphics_buffer_layouts;
using drivers::nvidia::OK;
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

// Answers the two internal controls with GR0's facts and sizes in NVIDIA's layout, recording each call
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
    call calls[MAX_CALLS] = {};
    uint32_t call_count = 0;

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

    // No function under test sends these, so a stray one shows as a failed call
    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override { return ERR_FIRMWARE; }

    int32_t control(rm::handle, uint32_t, void*, size_t) override { return ERR_FIRMWARE; }

    int32_t release(rm::handle) override { return ERR_FIRMWARE; }

    rm::handle new_handle() override { return 0; }

    rm::handle client() const override { return 0; }

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
