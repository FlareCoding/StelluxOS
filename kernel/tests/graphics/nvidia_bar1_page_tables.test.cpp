// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/bar1_page_tables.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/vram_allocator.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_bar1_page_tables);

using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::OK;
using drivers::nvidia::bar1_page_entry;
using drivers::nvidia::bar1_page_entry_offset;
using drivers::nvidia::bar1_page_table_bytes;
using drivers::nvidia::bar1_sparse_page_entry;
using drivers::nvidia::bar1_window_bytes;
using drivers::nvidia::check_console_region;
using drivers::nvidia::static_configuration;
using drivers::nvidia::video_memory_region;
using drivers::nvidia::vram_allocator;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
using drivers::nvidia::write_bar1_page_tables;

// NVIDIA's 4 KiB tables and pages, a table's 512 words (one entry each but in level 0, whose 256 entries take two), and
// which word of a level 0 entry is for a page table of larger pages and which is for a page table of 4 KiB pages
static constexpr uint64_t PAGE_BYTES          = 0x1000;
static constexpr uint64_t TABLE_WORDS         = 512;
static constexpr uint64_t TABLE_SPAN          = TABLE_WORDS * PAGE_BYTES;
static constexpr uint64_t LEVEL_0_ENTRIES     = 256;
static constexpr uint64_t LEVEL_0_ENTRY_WORDS = 2;
static constexpr uint64_t LARGE_PAGES_WORD    = 0;
static constexpr uint64_t SMALL_PAGES_WORD    = 1;

// The bit an entry's address field starts at, the 128 GiB reach of its 25 bits of 4 KiB units, and NVIDIA's sparse
// value, which sets the volatile bit, bit 3, in an entry naming nothing
static constexpr uint64_t ADDRESS_FIELD_SHIFT = 8;
static constexpr uint64_t ADDRESS_FIELD_REACH = 1ULL << 37;
static constexpr uint64_t SPARSE_ENTRY        = 0x8;

// The driver's layout: the level 2, level 1 and level 0 directories, then the page tables
static constexpr uint64_t DIRECTORY_PAGES = 3;

// The RTX 3080's boot console, 1440 rows of 16384 bytes, whose 5760 pages use 11 page tables and a quarter of a twelfth
static constexpr uint64_t RTX_3080_CONSOLE_BYTES = 0x1680000;
static constexpr uint64_t RTX_3080_PAGE_TABLES   = 12;

// A value no entry of the tables takes, filling each copy before a call, and one guard page after the copy that every
// call must leave holding it, so a write just past the tables fails a test instead of overwriting another allocation
static constexpr uint64_t UNWRITTEN_WORD = 0xA5A5A5A5A5A5A5A5;

// A copy of `pages` pages followed by the guard page, every word holding UNWRITTEN_WORD
static uint64_t* alloc_copy(uint64_t pages) {
    uint64_t words = (pages + 1) * TABLE_WORDS;
    uint64_t* copy = static_cast<uint64_t*>(heap::ualloc(words * sizeof(uint64_t)));
    if (copy == nullptr) {
        return nullptr;
    }

    for (uint64_t i = 0; i < words; i++) {
        copy[i] = UNWRITTEN_WORD;
    }

    return copy;
}

// How many words of the `pages` pages from page `first_page` of `copy` hold a value other than UNWRITTEN_WORD
static uint64_t written_words(const uint64_t* copy, uint64_t first_page, uint64_t pages) {
    uint64_t written = 0;
    for (uint64_t i = first_page * TABLE_WORDS; i < (first_page + pages) * TABLE_WORDS; i++) {
        written += copy[i] != UNWRITTEN_WORD;
    }

    return written;
}

// A directory entry naming the table at `offset` in video memory: the offset in 4 KiB units in the address field, and
// aperture 1 (video memory) in bits 2:1
static uint64_t directory_entry(uint64_t offset) {
    return (offset / PAGE_BYTES) << ADDRESS_FIELD_SHIFT | 0x2;
}

// A page table entry mapping the 4 KiB page at `offset` in video memory: the offset in 4 KiB units in the address
// field, aperture 0 (video memory) in bits 2:1, and its valid bit set in bit 0
static uint64_t page_entry(uint64_t offset) {
    return (offset / PAGE_BYTES) << ADDRESS_FIELD_SHIFT | 0x1;
}

// Writes tables at `tables_offset` for a 2 MiB window that a 2 MiB console fills, returning the writer's root entry
static uint64_t write_2_mib_window_tables(uint64_t* tables, uint64_t tables_offset) {
    return write_bar1_page_tables(tables, tables_offset, TABLE_SPAN, TABLE_SPAN);
}

// A size past 32 bits whose low 32 bits are a size the sizer accepts, so a cut to 32 bits would pass it
static constexpr uint64_t PAST_32_BITS_BYTES = (1ULL << 32) + PAGE_BYTES;

TEST(nvidia_bar1_page_tables, tables_take_three_directories_and_a_page_table_for_each_2_mib) {
    EXPECT_EQ(bar1_page_table_bytes(0), static_cast<uint64_t>(0));
    EXPECT_EQ(bar1_page_table_bytes(PAGE_BYTES), (DIRECTORY_PAGES + 1) * PAGE_BYTES);
    EXPECT_EQ(bar1_page_table_bytes(TABLE_SPAN), (DIRECTORY_PAGES + 1) * PAGE_BYTES);
    EXPECT_EQ(bar1_page_table_bytes(TABLE_SPAN + PAGE_BYTES), (DIRECTORY_PAGES + 2) * PAGE_BYTES);
    EXPECT_EQ(bar1_page_table_bytes(RTX_3080_CONSOLE_BYTES), (DIRECTORY_PAGES + RTX_3080_PAGE_TABLES) * PAGE_BYTES);
    EXPECT_EQ(bar1_page_table_bytes(LEVEL_0_ENTRIES * TABLE_SPAN), (DIRECTORY_PAGES + LEVEL_0_ENTRIES) * PAGE_BYTES);
    EXPECT_EQ(bar1_page_table_bytes(LEVEL_0_ENTRIES * TABLE_SPAN + PAGE_BYTES), static_cast<uint64_t>(0));
    EXPECT_EQ(bar1_page_table_bytes(PAGE_BYTES + 1), static_cast<uint64_t>(0));
    EXPECT_EQ(bar1_page_table_bytes(PAGE_BYTES + PAGE_BYTES / 2), static_cast<uint64_t>(0));
    EXPECT_EQ(bar1_page_table_bytes(PAST_32_BITS_BYTES), static_cast<uint64_t>(0));
}

// The RTX 3080's 256 MiB BAR1 and its window, 4 KiB smaller, whose 65535 pages take 128 page tables, and a BAR1 larger
// than the 512 MiB one level 0 directory covers
static constexpr uint64_t RTX_3080_BAR1_BYTES         = 0x10000000;
static constexpr uint64_t RTX_3080_WINDOW_BYTES       = 0x0FFFF000;
static constexpr uint64_t RTX_3080_WINDOW_PAGE_TABLES = 128;
static constexpr uint64_t LARGE_BAR1_BYTES            = 1ULL << 34;

TEST(nvidia_bar1_page_tables, bar1s_window_leaves_out_its_last_4_kib_and_stops_at_512_mib) {
    EXPECT_EQ(bar1_window_bytes(RTX_3080_BAR1_BYTES), RTX_3080_WINDOW_BYTES);
    EXPECT_EQ(bar1_window_bytes(0), static_cast<uint64_t>(0));
    EXPECT_EQ(bar1_window_bytes(PAGE_BYTES), static_cast<uint64_t>(0));
    EXPECT_EQ(bar1_window_bytes(2 * PAGE_BYTES), PAGE_BYTES);
    EXPECT_EQ(bar1_window_bytes(3 * PAGE_BYTES - 1), PAGE_BYTES);
    EXPECT_EQ(bar1_window_bytes(LEVEL_0_ENTRIES * TABLE_SPAN), LEVEL_0_ENTRIES * TABLE_SPAN - PAGE_BYTES);
    EXPECT_EQ(bar1_window_bytes(LEVEL_0_ENTRIES * TABLE_SPAN + PAGE_BYTES), LEVEL_0_ENTRIES * TABLE_SPAN);
    EXPECT_EQ(bar1_window_bytes(LARGE_BAR1_BYTES), LEVEL_0_ENTRIES * TABLE_SPAN);
    EXPECT_EQ(
        bar1_page_table_bytes(RTX_3080_WINDOW_BYTES),
        (DIRECTORY_PAGES + RTX_3080_WINDOW_PAGE_TABLES) * PAGE_BYTES
    );
}

// The video memory offset the test gives the tables, the start of the first region the RTX 3080's GSP firmware reports
// as usable with the boot console on the card
static constexpr uint64_t TABLES_OFFSET = 0x3F80000;

// How many words of tables written at `tables_offset` with `page_tables` page tables differ from the driver's layout,
// with the first `console_pages` pages of BAR1 mapped onto the same pages of video memory and every unused entry sparse
static uint64_t count_wrong_words(
    const uint64_t* tables,
    uint64_t tables_offset,
    uint64_t page_tables,
    uint64_t console_pages
) {
    const uint64_t* level_2 = tables;
    const uint64_t* level_1 = level_2 + TABLE_WORDS;
    const uint64_t* level_0 = level_1 + TABLE_WORDS;
    const uint64_t* page_table_words = level_0 + TABLE_WORDS;

    uint64_t wrong_words = 0;
    wrong_words += level_2[0] != directory_entry(tables_offset + PAGE_BYTES);
    wrong_words += level_1[0] != directory_entry(tables_offset + 2 * PAGE_BYTES);
    for (uint64_t i = 1; i < TABLE_WORDS; i++) {
        wrong_words += level_2[i] != SPARSE_ENTRY;
        wrong_words += level_1[i] != SPARSE_ENTRY;
    }

    for (uint64_t k = 0; k < LEVEL_0_ENTRIES; k++) {
        const uint64_t* entry = level_0 + k * LEVEL_0_ENTRY_WORDS;
        bool names_page_table = k < page_tables;
        uint64_t page_table_offset = tables_offset + (DIRECTORY_PAGES + k) * PAGE_BYTES;

        wrong_words += entry[LARGE_PAGES_WORD] != (names_page_table ? 0 : SPARSE_ENTRY);
        wrong_words += entry[SMALL_PAGES_WORD] != (names_page_table ? directory_entry(page_table_offset) : 0);
    }

    for (uint64_t page = 0; page < page_tables * TABLE_WORDS; page++) {
        wrong_words += page_table_words[page] != (page < console_pages ? page_entry(page * PAGE_BYTES) : SPARSE_ENTRY);
    }

    return wrong_words;
}

TEST(nvidia_bar1_page_tables, bar1_tables_map_bar1s_start_onto_the_same_video_memory_offsets_with_the_rest_sparse) {
    uint64_t table_pages = DIRECTORY_PAGES + RTX_3080_PAGE_TABLES;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    uint64_t root_entry = write_bar1_page_tables(tables, TABLES_OFFSET, RTX_3080_CONSOLE_BYTES, RTX_3080_CONSOLE_BYTES);

    uint64_t* level_2 = tables;
    uint64_t* level_1 = level_2 + TABLE_WORDS;
    uint64_t* level_0 = level_1 + TABLE_WORDS;
    uint64_t* page_tables = level_0 + TABLE_WORDS;
    uint64_t last_page_table_word = LEVEL_0_ENTRY_WORDS * (RTX_3080_PAGE_TABLES - 1) + SMALL_PAGES_WORD;

    EXPECT_EQ(root_entry, static_cast<uint64_t>(0x3F8002));
    EXPECT_EQ(level_2[0], static_cast<uint64_t>(0x3F8102));
    EXPECT_EQ(level_2[1], static_cast<uint64_t>(0x8));
    EXPECT_EQ(level_1[0], static_cast<uint64_t>(0x3F8202));
    EXPECT_EQ(level_0[LARGE_PAGES_WORD], static_cast<uint64_t>(0));
    EXPECT_EQ(level_0[SMALL_PAGES_WORD], static_cast<uint64_t>(0x3F8302));
    EXPECT_EQ(level_0[last_page_table_word], static_cast<uint64_t>(0x3F8E02));
    EXPECT_EQ(level_0[last_page_table_word + 1], static_cast<uint64_t>(0x8));
    EXPECT_EQ(page_tables[0], static_cast<uint64_t>(0x1));
    EXPECT_EQ(page_tables[0x1235], static_cast<uint64_t>(0x123501));
    EXPECT_EQ(page_tables[0x167F], static_cast<uint64_t>(0x167F01));
    EXPECT_EQ(page_tables[0x1680], static_cast<uint64_t>(0x8));

    uint64_t console_pages = RTX_3080_CONSOLE_BYTES / PAGE_BYTES;
    EXPECT_EQ(count_wrong_words(tables, TABLES_OFFSET, RTX_3080_PAGE_TABLES, console_pages), static_cast<uint64_t>(0));
    EXPECT_EQ(written_words(tables, table_pages, 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// A mapping's entries and the sparse entry are the ones the tables hold for the console and past it, at the offsets the
// tables give BAR1's pages
TEST(nvidia_bar1_page_tables, page_entries_and_their_offsets_match_the_tables_written_for_a_console) {
    uint64_t table_pages = DIRECTORY_PAGES + RTX_3080_PAGE_TABLES;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    ASSERT_NE(write_bar1_page_tables(tables, TABLES_OFFSET, RTX_3080_CONSOLE_BYTES, RTX_3080_CONSOLE_BYTES), 0u);

    static constexpr uint64_t CHECKED_PAGES[] = {0, 0x1235, 0x167F};
    for (uint64_t page : CHECKED_PAGES) {
        uint64_t bar1_offset = page * PAGE_BYTES;
        uint64_t entry_offset = bar1_page_entry_offset(TABLES_OFFSET, bar1_offset);
        EXPECT_EQ(entry_offset, TABLES_OFFSET + DIRECTORY_PAGES * PAGE_BYTES + page * sizeof(uint64_t));
        EXPECT_EQ(bar1_page_entry(bar1_offset), tables[(entry_offset - TABLES_OFFSET) / sizeof(uint64_t)]);
    }

    uint64_t past_console = bar1_page_entry_offset(TABLES_OFFSET, RTX_3080_CONSOLE_BYTES);
    EXPECT_EQ(bar1_sparse_page_entry(), tables[(past_console - TABLES_OFFSET) / sizeof(uint64_t)]);
    EXPECT_EQ(bar1_sparse_page_entry(), SPARSE_ENTRY);

    heap::ufree(tables);
}

TEST(nvidia_bar1_page_tables, a_page_entry_names_every_page_below_128_gib_and_refuses_the_rest) {
    uint64_t last_page = ADDRESS_FIELD_REACH - PAGE_BYTES;
    EXPECT_EQ(bar1_page_entry(last_page), (last_page >> 12) << ADDRESS_FIELD_SHIFT | 1);
    EXPECT_EQ(bar1_page_entry(ADDRESS_FIELD_REACH), 0u);
    EXPECT_EQ(bar1_page_entry(PAGE_BYTES + 8), 0u);
}

// Tables for the RTX 3080's window with its console map the console's pages and leave every page past the console
// sparse, BAR1's last 4 KiB included, in 128 page tables
TEST(nvidia_bar1_page_tables, tables_for_the_rtx_3080_window_map_the_console_and_leave_the_rest_of_bar1_sparse) {
    uint64_t table_pages = DIRECTORY_PAGES + RTX_3080_WINDOW_PAGE_TABLES;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    uint64_t root_entry = write_bar1_page_tables(tables, TABLES_OFFSET, RTX_3080_WINDOW_BYTES, RTX_3080_CONSOLE_BYTES);

    uint64_t* page_tables = tables + DIRECTORY_PAGES * TABLE_WORDS;
    uint64_t console_pages = RTX_3080_CONSOLE_BYTES / PAGE_BYTES;

    EXPECT_EQ(root_entry, directory_entry(TABLES_OFFSET));
    EXPECT_EQ(page_tables[console_pages - 1], page_entry(RTX_3080_CONSOLE_BYTES - PAGE_BYTES));
    EXPECT_EQ(page_tables[RTX_3080_BAR1_BYTES / PAGE_BYTES - 1], SPARSE_ENTRY);
    EXPECT_EQ(
        count_wrong_words(tables, TABLES_OFFSET, RTX_3080_WINDOW_PAGE_TABLES, console_pages),
        static_cast<uint64_t>(0)
    );
    EXPECT_EQ(written_words(tables, table_pages, 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// The start of the one usable region the RTX 3080's GSP firmware reports with no boot console on the card
static constexpr uint64_t NO_CONSOLE_TABLES_OFFSET = 0x2A40000;

// Without a console, every page entry of the window is sparse while every level 0 entry the window uses still names its
// page table, so a later mapping only writes page entries
TEST(nvidia_bar1_page_tables, without_a_console_every_page_entry_of_the_window_is_sparse) {
    uint64_t table_pages = DIRECTORY_PAGES + RTX_3080_WINDOW_PAGE_TABLES;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    uint64_t root_entry = write_bar1_page_tables(tables, NO_CONSOLE_TABLES_OFFSET, RTX_3080_WINDOW_BYTES, 0);

    EXPECT_EQ(root_entry, static_cast<uint64_t>(0x2A4002));
    EXPECT_EQ(
        count_wrong_words(tables, NO_CONSOLE_TABLES_OFFSET, RTX_3080_WINDOW_PAGE_TABLES, 0),
        static_cast<uint64_t>(0)
    );
    EXPECT_EQ(written_words(tables, table_pages, 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// A console 4 KiB past 2 MiB, so it ends inside a 64 KiB or 128 KiB page, the rest of which NVIDIA's driver maps onto
// the console's first pages and these tables make sparse
static constexpr uint64_t SPLIT_PAGE_CONSOLE_BYTES = TABLE_SPAN + PAGE_BYTES;

TEST(nvidia_bar1_page_tables, page_entries_past_a_console_ending_inside_a_larger_page_are_sparse) {
    uint64_t table_pages = DIRECTORY_PAGES + 2;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    uint64_t root_entry = write_bar1_page_tables(
        tables,
        TABLES_OFFSET,
        SPLIT_PAGE_CONSOLE_BYTES,
        SPLIT_PAGE_CONSOLE_BYTES
    );

    uint64_t* page_tables = tables + DIRECTORY_PAGES * TABLE_WORDS;

    EXPECT_EQ(root_entry, directory_entry(TABLES_OFFSET));
    EXPECT_EQ(page_tables[TABLE_WORDS], page_entry(TABLE_SPAN));
    EXPECT_EQ(page_tables[TABLE_WORDS + 1], SPARSE_ENTRY);
    EXPECT_EQ(page_tables[2 * TABLE_WORDS - 1], SPARSE_ENTRY);
    EXPECT_EQ(written_words(tables, table_pages, 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// Four 4 KiB tables ending at 128 GiB, the reach of an entry's 25-bit address field in 4 KiB units, so every directory
// entry naming them sets bit 32, which a cut to 32 bits would lose
static constexpr uint64_t TOP_TABLES_OFFSET = ADDRESS_FIELD_REACH - (DIRECTORY_PAGES + 1) * PAGE_BYTES;

TEST(nvidia_bar1_page_tables, tables_ending_at_the_address_fields_reach_keep_every_bit) {
    uint64_t table_pages = DIRECTORY_PAGES + 1;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    uint64_t root_entry = write_2_mib_window_tables(tables, TOP_TABLES_OFFSET);

    uint64_t* level_2 = tables;
    uint64_t* level_1 = level_2 + TABLE_WORDS;
    uint64_t* level_0 = level_1 + TABLE_WORDS;
    uint64_t* page_table = level_0 + TABLE_WORDS;

    EXPECT_EQ(root_entry, static_cast<uint64_t>(0x1FFFFFC02));
    EXPECT_EQ(level_2[0], static_cast<uint64_t>(0x1FFFFFD02));
    EXPECT_EQ(level_1[0], static_cast<uint64_t>(0x1FFFFFE02));
    EXPECT_EQ(level_0[SMALL_PAGES_WORD], static_cast<uint64_t>(0x1FFFFFF02));
    EXPECT_EQ(level_0[LEVEL_0_ENTRY_WORDS + SMALL_PAGES_WORD], static_cast<uint64_t>(0));
    EXPECT_EQ(page_table[TABLE_WORDS - 1], static_cast<uint64_t>(0x1FF01));
    EXPECT_EQ(written_words(tables, table_pages, 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// Offsets the writer must take: right after a 2 MiB console, and 4 KiB below the reach test's tables, an odd multiple
// of 4 KiB that a check for 8 KiB alignment would refuse
static constexpr uint64_t AFTER_CONSOLE_OFFSET = TABLE_SPAN;
static constexpr uint64_t ODD_PAGES_OFFSET     = TOP_TABLES_OFFSET - PAGE_BYTES;

TEST(nvidia_bar1_page_tables, tables_right_after_the_console_or_at_an_odd_multiple_of_4_kib_are_named) {
    uint64_t table_pages = DIRECTORY_PAGES + 1;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    EXPECT_EQ(write_2_mib_window_tables(tables, AFTER_CONSOLE_OFFSET), static_cast<uint64_t>(0x20002));
    EXPECT_EQ(write_2_mib_window_tables(tables, ODD_PAGES_OFFSET), static_cast<uint64_t>(0x1FFFFFB02));
    EXPECT_EQ(written_words(tables, table_pages, 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// Entry 0 of the RTX 3080's GSP firmware's root page directory as read on the card, naming the firmware's level 2
// directory at 0x2744A8000, which the writer must name the same way
static constexpr uint64_t RTX_3080_LEVEL_2_OFFSET = 0x2744A8000;
static constexpr uint64_t RTX_3080_ROOT_ENTRY     = 0x2744A802;

TEST(nvidia_bar1_page_tables, root_entry_matches_the_rtx_3080_firmwares_entry_for_the_same_offset) {
    uint64_t table_pages = DIRECTORY_PAGES + 1;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    EXPECT_EQ(write_2_mib_window_tables(tables, RTX_3080_LEVEL_2_OFFSET), RTX_3080_ROOT_ENTRY);
    EXPECT_EQ(written_words(tables, table_pages, 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// The last 4 KiB of a 2 MiB console, where tables would overwrite the console, and an offset 4 KiB below 2^64, so its
// sum with the tables' bytes wraps to a small offset
static constexpr uint64_t INSIDE_CONSOLE_OFFSET  = TABLE_SPAN - PAGE_BYTES;
static constexpr uint64_t WRAPPING_TABLES_OFFSET = ~(PAGE_BYTES - 1);

TEST(nvidia_bar1_page_tables, an_offset_or_size_the_tables_cannot_take_is_refused_leaving_the_copy_unwritten) {
    uint64_t table_pages = DIRECTORY_PAGES + 1;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    EXPECT_EQ(write_2_mib_window_tables(tables, TABLES_OFFSET + 1), static_cast<uint64_t>(0));
    EXPECT_EQ(write_2_mib_window_tables(tables, TABLES_OFFSET + PAGE_BYTES / 2), static_cast<uint64_t>(0));
    EXPECT_EQ(write_2_mib_window_tables(tables, TOP_TABLES_OFFSET + PAGE_BYTES), static_cast<uint64_t>(0));
    EXPECT_EQ(write_2_mib_window_tables(tables, WRAPPING_TABLES_OFFSET), static_cast<uint64_t>(0));
    EXPECT_EQ(write_2_mib_window_tables(tables, INSIDE_CONSOLE_OFFSET), static_cast<uint64_t>(0));
    EXPECT_EQ(write_bar1_page_tables(tables, TABLES_OFFSET, 0, 0), static_cast<uint64_t>(0));
    EXPECT_EQ(write_bar1_page_tables(tables, TABLES_OFFSET, TABLE_SPAN + 1, TABLE_SPAN), static_cast<uint64_t>(0));
    EXPECT_EQ(write_bar1_page_tables(tables, TABLES_OFFSET, TABLE_SPAN, PAGE_BYTES + 1), static_cast<uint64_t>(0));
    EXPECT_EQ(
        write_bar1_page_tables(tables, TABLES_OFFSET, TABLE_SPAN, TABLE_SPAN + PAGE_BYTES),
        static_cast<uint64_t>(0)
    );
    EXPECT_EQ(written_words(tables, 0, table_pages + 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// The RTX 3080's 10 GiB, its one usable region's end and BAR1's root page directory, which lies in the reserved region
// after it
static constexpr uint64_t RTX_3080_VIDEO_MEMORY_BYTES = 0x280000000;
static constexpr uint64_t RTX_3080_USABLE_END         = 0x2728E0000;
static constexpr uint64_t RTX_3080_BAR1_ROOT          = 0x2744A9000;

// A region the firmware reserves whole, of performance 0, unprotected and supporting neither scanout nor compression
static video_memory_region rtx_3080_reserved_region(uint64_t offset, uint64_t size) {
    return {.offset = offset, .size = size, .reserved_bytes = size, .is_reserved = true};
}

// The RTX 3080's static configuration with the boot console on the card: region 0 holds the console, region 2 is the
// one usable region and region 3 holds BAR1's root page directory, for a test to copy and edit for its own case
static static_configuration rtx_3080_console_configuration() {
    static_configuration configuration = {};
    configuration.regions[0] = rtx_3080_reserved_region(0, RTX_3080_CONSOLE_BYTES);
    configuration.regions[1] = rtx_3080_reserved_region(RTX_3080_CONSOLE_BYTES, 0x2900000);
    configuration.regions[2] = {
        .offset = TABLES_OFFSET,
        .size = RTX_3080_USABLE_END - TABLES_OFFSET,
        .performance = 10,
        .supports_scanout = true,
        .supports_compression = true,
    };

    configuration.regions[3] = {
        .offset = RTX_3080_USABLE_END,
        .size = 0x30B0000,
        .reserved_bytes = 0x30B0000,
        .performance = 10,
        .is_reserved = true,
        .supports_scanout = true,
        .supports_compression = true,
    };

    configuration.regions[4] = rtx_3080_reserved_region(0x275990000, 0x70000);
    configuration.regions[5] = rtx_3080_reserved_region(0x275A00000, 0xA600000);
    configuration.region_count = 6;
    configuration.bar1_page_directory_offset = RTX_3080_BAR1_ROOT;
    return configuration;
}

// The bytes of BAR1's tables for the RTX 3080's window, and a console size no call gives, which a refusal must leave as
// it is
static constexpr uint64_t RTX_3080_TABLES_BYTES = (DIRECTORY_PAGES + RTX_3080_WINDOW_PAGE_TABLES) * PAGE_BYTES;
static constexpr uint64_t UNWRITTEN_BYTES       = 1;

// The console is region 0 whole, holding exactly the reserved bytes or more, and the allocator hands out the tables'
// video memory at the start of the usable region, the offset the writer's tests give the tables
TEST(nvidia_bar1_page_tables, rtx_3080_console_is_region_0_whole_and_bar1s_tables_lie_at_the_usable_regions_start) {
    static_configuration configuration = rtx_3080_console_configuration();
    uint64_t console_bytes = UNWRITTEN_BYTES;

    ASSERT_EQ(check_console_region(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, console_bytes), OK);
    EXPECT_EQ(console_bytes, RTX_3080_CONSOLE_BYTES);

    console_bytes = UNWRITTEN_BYTES;
    ASSERT_EQ(
        check_console_region(configuration, RTX_3080_CONSOLE_BYTES - TABLE_SPAN, RTX_3080_BAR1_BYTES, console_bytes),
        OK
    );
    EXPECT_EQ(console_bytes, RTX_3080_CONSOLE_BYTES);

    vram_allocator allocator;
    ASSERT_EQ(allocator.init(configuration, RTX_3080_VIDEO_MEMORY_BYTES), OK);

    vram_range tables_range = {};
    uint64_t tables_bytes = bar1_page_table_bytes(RTX_3080_WINDOW_BYTES);
    ASSERT_EQ(allocator.allocate(tables_bytes, PAGE_BYTES, vram_use::general, tables_range), OK);
    EXPECT_EQ(tables_range.offset, TABLES_OFFSET);
    EXPECT_EQ(tables_range.size, RTX_3080_TABLES_BYTES);
}

// Region 0 reported usable is refused, since a video memory allocation could then overwrite the console
TEST(nvidia_bar1_page_tables, region_0_is_refused_unless_it_is_reserved) {
    static_configuration configuration = rtx_3080_console_configuration();
    configuration.regions[0].reserved_bytes = 0;
    configuration.regions[0].is_reserved    = false;
    uint64_t console_bytes = UNWRITTEN_BYTES;

    EXPECT_EQ(
        check_console_region(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, console_bytes),
        ERR_BAD_STATE
    );
    EXPECT_EQ(console_bytes, UNWRITTEN_BYTES);
}

// Region 0 is taken when it fills the window of a BAR1 4 KiB larger and refused when it fills BAR1, and in a 16 GiB
// BAR1 it is taken up to the window's 512 MiB and refused past it
TEST(nvidia_bar1_page_tables, region_0_is_refused_unless_it_fits_bar1s_window) {
    static_configuration configuration = rtx_3080_console_configuration();
    uint64_t console_bytes = UNWRITTEN_BYTES;
    uint64_t bar1_bytes = RTX_3080_CONSOLE_BYTES + PAGE_BYTES;

    ASSERT_EQ(check_console_region(configuration, RTX_3080_CONSOLE_BYTES, bar1_bytes, console_bytes), OK);
    EXPECT_EQ(console_bytes, RTX_3080_CONSOLE_BYTES);

    console_bytes = UNWRITTEN_BYTES;
    bar1_bytes = RTX_3080_CONSOLE_BYTES;
    EXPECT_EQ(check_console_region(configuration, RTX_3080_CONSOLE_BYTES, bar1_bytes, console_bytes), ERR_BAD_STATE);
    EXPECT_EQ(console_bytes, UNWRITTEN_BYTES);

    configuration.regions[0] = rtx_3080_reserved_region(0, LEVEL_0_ENTRIES * TABLE_SPAN);
    ASSERT_EQ(check_console_region(configuration, RTX_3080_CONSOLE_BYTES, LARGE_BAR1_BYTES, console_bytes), OK);
    EXPECT_EQ(console_bytes, LEVEL_0_ENTRIES * TABLE_SPAN);

    console_bytes = UNWRITTEN_BYTES;
    configuration.regions[0] = rtx_3080_reserved_region(0, LEVEL_0_ENTRIES * TABLE_SPAN + PAGE_BYTES);
    EXPECT_EQ(
        check_console_region(configuration, RTX_3080_CONSOLE_BYTES, LARGE_BAR1_BYTES, console_bytes),
        ERR_BAD_STATE
    );
    EXPECT_EQ(console_bytes, UNWRITTEN_BYTES);
}

// Region 0 starting past 0, smaller than the bytes reserved for the console, or ending inside a 4 KiB page is refused
TEST(nvidia_bar1_page_tables, region_0_is_refused_unless_it_starts_at_0_and_holds_the_reserved_bytes_in_whole_pages) {
    static_configuration configuration = rtx_3080_console_configuration();
    uint64_t console_bytes = UNWRITTEN_BYTES;

    configuration.regions[0].offset = PAGE_BYTES;
    EXPECT_EQ(
        check_console_region(configuration, RTX_3080_CONSOLE_BYTES - TABLE_SPAN, RTX_3080_BAR1_BYTES, console_bytes),
        ERR_BAD_STATE
    );
    configuration.regions[0].offset = 0;
    EXPECT_EQ(
        check_console_region(configuration, RTX_3080_CONSOLE_BYTES + PAGE_BYTES, RTX_3080_BAR1_BYTES, console_bytes),
        ERR_BAD_STATE
    );
    configuration.regions[0].size = RTX_3080_CONSOLE_BYTES + PAGE_BYTES / 2;
    EXPECT_EQ(
        check_console_region(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, console_bytes),
        ERR_BAD_STATE
    );
    EXPECT_EQ(console_bytes, UNWRITTEN_BYTES);
}
