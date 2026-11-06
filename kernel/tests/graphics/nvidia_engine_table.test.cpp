// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/engine_table.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/nvidia.h"

TEST_SUITE(nvidia_engine_table);

using drivers::nvidia::engine_info;
using drivers::nvidia::engine_table;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::OK;
namespace rm = drivers::nvidia::rm;

// NVIDIA's control number, page layout and engine types, restated here so the tests hold the reads to NVIDIA's layout
// rather than to the driver's
static constexpr uint32_t NVIDIA_GET_DEVICE_INFO_TABLE = 0x20801112;
static constexpr size_t   NVIDIA_PAGE_BYTES            = 3212;
static constexpr uint32_t NVIDIA_PAGE_ENTRIES          = 32;
static constexpr size_t   NVIDIA_ENTRY_BYTES           = 100;
static constexpr size_t   NVIDIA_FIRST_ENTRY_OFFSET    = 12;
static constexpr uint32_t NVIDIA_ENGINE_TYPE_WORD      = 2;
static constexpr uint32_t NVIDIA_RUNLIST_WORD          = 3;
static constexpr uint32_t NVIDIA_CHANNEL_RAM_WORD      = 14;
static constexpr uint32_t GR0                          = 0x1;
static constexpr uint32_t CE0                          = 0x9;
static constexpr uint32_t CE1                          = 0xA;
static constexpr uint32_t CE2                          = 0xB;
static constexpr uint32_t CE3                          = 0xC;
static constexpr uint32_t CE4                          = 0xD;
static constexpr uint32_t NVDEC0                       = 0x1D;

static constexpr rm::handle TEST_SUBDEVICE = 0x00050002;

// Seven engines in the shape of an RTX 3080's table, of which the card logs only CE2 on runlist 1: the graphics engine
// and two copy engines on runlist 0, then copy engines and a video decoder on runlists of their own
static constexpr engine_info RTX_3080_ENGINES[] = {
    {.type = GR0, .runlist = 0, .channel_ram = 0},
    {.type = CE0, .runlist = 0, .channel_ram = 0},
    {.type = CE1, .runlist = 0, .channel_ram = 0},
    {.type = CE2, .runlist = 1, .channel_ram = 0},
    {.type = CE3, .runlist = 2, .channel_ram = 0},
    {.type = CE4, .runlist = 8, .channel_ram = 0},
    {.type = NVDEC0, .runlist = 3, .channel_ram = 0},
};

// A firmware answering the device info table control from `engines`, a page of 32 per call, and recording each call's
// object, command, size and first entry asked for. `entry_count_override` replaces every page's count when set
class table_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 16;

    struct call {
        rm::handle object;
        uint32_t   command;
        size_t     size;
        uint32_t   first_entry;
    };

    const engine_info* engines = nullptr;
    uint32_t           engine_count = 0;
    bool               always_has_more = false;
    bool               overrides_entry_count = false;
    uint32_t           entry_count_override = 0;
    int32_t            result = OK;
    call               calls[MAX_CALLS] = {};
    uint32_t           call_count = 0;

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        auto* bytes = static_cast<uint8_t*>(params);
        uint32_t first_entry = 0;
        string::memcpy(&first_entry, bytes, sizeof(first_entry));

        if (call_count < MAX_CALLS) {
            calls[call_count] = {.object = object, .command = command, .size = size, .first_entry = first_entry};
        }

        call_count++;

        if (result != OK) {
            return result;
        }

        uint32_t count = 0;
        for (uint32_t i = first_entry; i < engine_count && count < NVIDIA_PAGE_ENTRIES; i++) {
            uint8_t* entry = bytes + NVIDIA_FIRST_ENTRY_OFFSET + count * NVIDIA_ENTRY_BYTES;
            string::memcpy(entry + NVIDIA_ENGINE_TYPE_WORD * 4, &engines[i].type, 4);
            string::memcpy(entry + NVIDIA_RUNLIST_WORD * 4, &engines[i].runlist, 4);
            string::memcpy(entry + NVIDIA_CHANNEL_RAM_WORD * 4, &engines[i].channel_ram, 4);
            count++;
        }

        uint32_t reported = overrides_entry_count ? entry_count_override : count;
        uint8_t has_more = always_has_more || first_entry + NVIDIA_PAGE_ENTRIES < engine_count;
        string::memcpy(bytes + 4, &reported, sizeof(reported));
        bytes[8] = has_more;
        return OK;
    }

    // No function under test sends these, so a stray one shows as a crash-free no-op
    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override {
        return OK;
    }

    int32_t release(rm::handle) override {
        return OK;
    }

    rm::handle new_handle() override {
        return 0;
    }

    rm::handle client() const override {
        return 0;
    }

    int32_t internal_control(uint32_t, void*, size_t) override {
        return OK;
    }

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

    void set_error_sink(drivers::nvidia::firmware_error_sink*) override {}
};

// The 3080's table fits one page, asked of the subdevice at entry 0, and its first asynchronous copy engine is CE2,
// since CE0 and CE1 share the graphics engine's runlist
TEST(nvidia_engine_table, rtx_3080_table_gives_ce2) {
    table_firmware fw;
    fw.engines = RTX_3080_ENGINES;
    fw.engine_count = sizeof(RTX_3080_ENGINES) / sizeof(RTX_3080_ENGINES[0]);
    engine_table table;

    ASSERT_EQ(table.read(fw, TEST_SUBDEVICE), OK);

    ASSERT_EQ(fw.call_count, 1u);
    EXPECT_EQ(fw.calls[0].object, TEST_SUBDEVICE);
    EXPECT_EQ(fw.calls[0].command, NVIDIA_GET_DEVICE_INFO_TABLE);
    EXPECT_EQ(fw.calls[0].size, NVIDIA_PAGE_BYTES);
    EXPECT_EQ(fw.calls[0].first_entry, 0u);
    EXPECT_EQ(table.count(), fw.engine_count);

    const engine_info* copy = table.first_async_copy_engine();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->type, CE2);
    EXPECT_EQ(copy->runlist, 1u);
}

// An engine keeps its runlist's channel RAM offset from word 14 of its entry
TEST(nvidia_engine_table, an_engine_keeps_its_runlists_channel_ram_from_word_14) {
    static constexpr engine_info engines[] = {
        {.type = GR0, .runlist = 0, .channel_ram = 0x412000},
        {.type = CE2, .runlist = 1, .channel_ram = 0x416000},
    };

    table_firmware fw;
    fw.engines = engines;
    fw.engine_count = 2;
    engine_table table;

    ASSERT_EQ(table.read(fw, TEST_SUBDEVICE), OK);

    const engine_info* copy = table.first_async_copy_engine();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->channel_ram, 0x416000u);
}

// A table of 40 engines takes two pages, the second asked from entry 32, and keeps all 40, the last of them from the
// second page
TEST(nvidia_engine_table, second_page_starts_at_entry_32) {
    static engine_info engines[40] = {};
    for (uint32_t i = 0; i < 40; i++) {
        engines[i] = {.type = NVDEC0, .runlist = 3, .channel_ram = 0};
    }

    engines[0] = {.type = GR0, .runlist = 0, .channel_ram = 0};
    engines[39] = {.type = CE3, .runlist = 2, .channel_ram = 0};

    table_firmware fw;
    fw.engines = engines;
    fw.engine_count = 40;
    engine_table table;

    ASSERT_EQ(table.read(fw, TEST_SUBDEVICE), OK);

    ASSERT_EQ(fw.call_count, 2u);
    EXPECT_EQ(fw.calls[1].first_entry, NVIDIA_PAGE_ENTRIES);
    EXPECT_EQ(table.count(), 40u);

    const engine_info* copy = table.first_async_copy_engine();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->type, CE3);
}

// The lowest asynchronous copy engine type wins, even when a higher one comes first
TEST(nvidia_engine_table, lowest_async_copy_engine_wins) {
    static constexpr engine_info engines[] = {
        {.type = CE4, .runlist = 8, .channel_ram = 0},
        {.type = GR0, .runlist = 0, .channel_ram = 0},
        {.type = CE3, .runlist = 2, .channel_ram = 0},
    };

    table_firmware fw;
    fw.engines = engines;
    fw.engine_count = 3;
    engine_table table;

    ASSERT_EQ(table.read(fw, TEST_SUBDEVICE), OK);

    const engine_info* copy = table.first_async_copy_engine();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->type, CE3);
}

// The asynchronous copy engine is the table's first entry and the graphics engine its last, so both walks read both
// ends: missing GR0 would make CE0 asynchronous and lower, and missing CE2 would leave none
TEST(nvidia_engine_table, walks_read_the_first_and_last_entries) {
    static constexpr engine_info engines[] = {
        {.type = CE2, .runlist = 1, .channel_ram = 0},
        {.type = CE0, .runlist = 0, .channel_ram = 0},
        {.type = GR0, .runlist = 0, .channel_ram = 0},
    };

    table_firmware fw;
    fw.engines = engines;
    fw.engine_count = 3;
    engine_table table;

    ASSERT_EQ(table.read(fw, TEST_SUBDEVICE), OK);

    const engine_info* copy = table.first_async_copy_engine();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->type, CE2);
}

// Copy engines that all share the graphics engine's runlist leave no asynchronous copy engine
TEST(nvidia_engine_table, copy_engines_on_the_graphics_runlist_give_none) {
    static constexpr engine_info engines[] = {
        {.type = GR0, .runlist = 0, .channel_ram = 0},
        {.type = CE0, .runlist = 0, .channel_ram = 0},
        {.type = CE1, .runlist = 0, .channel_ram = 0},
        {.type = NVDEC0, .runlist = 3, .channel_ram = 0},
    };

    table_firmware fw;
    fw.engines = engines;
    fw.engine_count = 4;
    engine_table table;

    ASSERT_EQ(table.read(fw, TEST_SUBDEVICE), OK);

    EXPECT_EQ(table.first_async_copy_engine(), nullptr);
}

// A page counting more entries than it holds is refused, leaving the table empty
TEST(nvidia_engine_table, page_counting_too_many_entries_is_refused) {
    table_firmware fw;
    fw.engines = RTX_3080_ENGINES;
    fw.engine_count = sizeof(RTX_3080_ENGINES) / sizeof(RTX_3080_ENGINES[0]);
    fw.overrides_entry_count = true;
    fw.entry_count_override = NVIDIA_PAGE_ENTRIES + 1;
    engine_table table;

    EXPECT_EQ(table.read(fw, TEST_SUBDEVICE), ERR_BAD_MESSAGE);

    EXPECT_EQ(table.count(), 0u);
    EXPECT_EQ(table.first_async_copy_engine(), nullptr);
}

// A table whose every page says more follow is refused once it runs past 256 entries, after its eight pages
TEST(nvidia_engine_table, table_past_256_entries_is_refused) {
    table_firmware fw;
    fw.engines = RTX_3080_ENGINES;
    fw.engine_count = sizeof(RTX_3080_ENGINES) / sizeof(RTX_3080_ENGINES[0]);
    fw.always_has_more = true;
    engine_table table;

    EXPECT_EQ(table.read(fw, TEST_SUBDEVICE), ERR_BAD_MESSAGE);

    EXPECT_EQ(fw.call_count, 8u);
    EXPECT_EQ(table.count(), 0u);
}

// A failed control returns its error and leaves an earlier table's engines dropped
TEST(nvidia_engine_table, failed_control_leaves_the_table_empty) {
    table_firmware fw;
    fw.engines = RTX_3080_ENGINES;
    fw.engine_count = sizeof(RTX_3080_ENGINES) / sizeof(RTX_3080_ENGINES[0]);
    engine_table table;
    ASSERT_EQ(table.read(fw, TEST_SUBDEVICE), OK);

    fw.result = ERR_TIMEOUT;

    EXPECT_EQ(table.read(fw, TEST_SUBDEVICE), ERR_TIMEOUT);

    EXPECT_EQ(table.count(), 0u);
    EXPECT_EQ(table.first_async_copy_engine(), nullptr);
}
