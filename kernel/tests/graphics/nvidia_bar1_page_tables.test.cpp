// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/bar1_page_tables.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_bar1_page_tables);

using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::OK;
using drivers::nvidia::console_page_table_bytes;
using drivers::nvidia::console_tables_placement;
using drivers::nvidia::place_console_page_tables;
using drivers::nvidia::static_configuration;
using drivers::nvidia::write_console_page_tables;

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

// A size past 32 bits whose low 32 bits are a size the sizer accepts, so a cut to 32 bits would pass it
static constexpr uint64_t PAST_32_BITS_BYTES = (1ULL << 32) + PAGE_BYTES;

TEST(nvidia_bar1_page_tables, tables_take_three_directories_and_a_page_table_for_each_2_mib) {
    EXPECT_EQ(console_page_table_bytes(0), static_cast<uint64_t>(0));
    EXPECT_EQ(console_page_table_bytes(PAGE_BYTES), (DIRECTORY_PAGES + 1) * PAGE_BYTES);
    EXPECT_EQ(console_page_table_bytes(TABLE_SPAN), (DIRECTORY_PAGES + 1) * PAGE_BYTES);
    EXPECT_EQ(console_page_table_bytes(TABLE_SPAN + PAGE_BYTES), (DIRECTORY_PAGES + 2) * PAGE_BYTES);
    EXPECT_EQ(console_page_table_bytes(RTX_3080_CONSOLE_BYTES), (DIRECTORY_PAGES + RTX_3080_PAGE_TABLES) * PAGE_BYTES);
    EXPECT_EQ(console_page_table_bytes(LEVEL_0_ENTRIES * TABLE_SPAN), (DIRECTORY_PAGES + LEVEL_0_ENTRIES) * PAGE_BYTES);
    EXPECT_EQ(console_page_table_bytes(LEVEL_0_ENTRIES * TABLE_SPAN + PAGE_BYTES), static_cast<uint64_t>(0));
    EXPECT_EQ(console_page_table_bytes(PAGE_BYTES + 1), static_cast<uint64_t>(0));
    EXPECT_EQ(console_page_table_bytes(PAGE_BYTES + PAGE_BYTES / 2), static_cast<uint64_t>(0));
    EXPECT_EQ(console_page_table_bytes(PAST_32_BITS_BYTES), static_cast<uint64_t>(0));
}

// The video memory offset the test gives the tables, the start of the first region the RTX 3080's GSP firmware reports
// as usable
static constexpr uint64_t TABLES_OFFSET = 0x3F80000;

TEST(nvidia_bar1_page_tables, console_tables_map_bar1s_start_onto_the_same_video_memory_offsets_with_the_rest_sparse) {
    uint64_t table_pages = DIRECTORY_PAGES + RTX_3080_PAGE_TABLES;
    uint64_t* tables = alloc_copy(table_pages);
    ASSERT_NOT_NULL(tables);

    uint64_t root_entry = write_console_page_tables(tables, TABLES_OFFSET, RTX_3080_CONSOLE_BYTES);

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

    uint64_t wrong_words = 0;
    for (uint64_t i = 1; i < TABLE_WORDS; i++) {
        wrong_words += level_2[i] != SPARSE_ENTRY;
        wrong_words += level_1[i] != SPARSE_ENTRY;
    }

    for (uint64_t k = 0; k < LEVEL_0_ENTRIES; k++) {
        uint64_t* entry = level_0 + k * LEVEL_0_ENTRY_WORDS;
        bool names_page_table = k < RTX_3080_PAGE_TABLES;
        uint64_t page_table_offset = TABLES_OFFSET + (DIRECTORY_PAGES + k) * PAGE_BYTES;

        wrong_words += entry[LARGE_PAGES_WORD] != (names_page_table ? 0 : SPARSE_ENTRY);
        wrong_words += entry[SMALL_PAGES_WORD] != (names_page_table ? directory_entry(page_table_offset) : 0);
    }

    uint64_t console_pages = RTX_3080_CONSOLE_BYTES / PAGE_BYTES;
    for (uint64_t page = 0; page < RTX_3080_PAGE_TABLES * TABLE_WORDS; page++) {
        wrong_words += page_tables[page] != (page < console_pages ? page_entry(page * PAGE_BYTES) : SPARSE_ENTRY);
    }

    EXPECT_EQ(wrong_words, static_cast<uint64_t>(0));
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

    uint64_t root_entry = write_console_page_tables(tables, TABLES_OFFSET, SPLIT_PAGE_CONSOLE_BYTES);

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

    uint64_t root_entry = write_console_page_tables(tables, TOP_TABLES_OFFSET, TABLE_SPAN);

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

    EXPECT_EQ(write_console_page_tables(tables, AFTER_CONSOLE_OFFSET, TABLE_SPAN), static_cast<uint64_t>(0x20002));
    EXPECT_EQ(write_console_page_tables(tables, ODD_PAGES_OFFSET, TABLE_SPAN), static_cast<uint64_t>(0x1FFFFFB02));
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

    EXPECT_EQ(write_console_page_tables(tables, RTX_3080_LEVEL_2_OFFSET, TABLE_SPAN), RTX_3080_ROOT_ENTRY);
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

    EXPECT_EQ(write_console_page_tables(tables, TABLES_OFFSET + 1, TABLE_SPAN), static_cast<uint64_t>(0));
    EXPECT_EQ(write_console_page_tables(tables, TABLES_OFFSET + PAGE_BYTES / 2, TABLE_SPAN), static_cast<uint64_t>(0));
    EXPECT_EQ(write_console_page_tables(tables, TOP_TABLES_OFFSET + PAGE_BYTES, TABLE_SPAN), static_cast<uint64_t>(0));
    EXPECT_EQ(write_console_page_tables(tables, WRAPPING_TABLES_OFFSET, TABLE_SPAN), static_cast<uint64_t>(0));
    EXPECT_EQ(write_console_page_tables(tables, INSIDE_CONSOLE_OFFSET, TABLE_SPAN), static_cast<uint64_t>(0));
    EXPECT_EQ(write_console_page_tables(tables, TABLES_OFFSET, TABLE_SPAN + 1), static_cast<uint64_t>(0));
    EXPECT_EQ(written_words(tables, 0, table_pages + 1), static_cast<uint64_t>(0));

    heap::ufree(tables);
}

// The first three of the six regions the RTX 3080's GSP firmware reports: region 0 for the console, the rest of what it
// reserves below its one usable region, and that usable region, with BAR1's root page directory reserved past it
static constexpr uint64_t RTX_3080_USABLE_END = 0x2728E0000;
static constexpr uint64_t RTX_3080_BAR1_ROOT  = 0x2744A9000;

// A static configuration holding those regions and that root, for a test to copy and edit for its own case
static static_configuration rtx_3080_configuration() {
    static_configuration configuration = {};
    configuration.regions[0] = {.offset = 0, .size = RTX_3080_CONSOLE_BYTES, .is_reserved = true};
    configuration.regions[1] = {.offset = RTX_3080_CONSOLE_BYTES, .size = 0x2900000, .is_reserved = true};
    configuration.regions[2] = {.offset = TABLES_OFFSET, .size = RTX_3080_USABLE_END - TABLES_OFFSET};
    configuration.region_count = 3;
    configuration.bar1_page_directory_offset = RTX_3080_BAR1_ROOT;
    return configuration;
}

// The bytes of the RTX 3080's console's tables and of its BAR1, and a placement no call makes, which a refusal must
// leave as it is
static constexpr uint64_t RTX_3080_TABLES_BYTES = (DIRECTORY_PAGES + RTX_3080_PAGE_TABLES) * PAGE_BYTES;
static constexpr uint64_t RTX_3080_BAR1_BYTES   = 0x10000000;
static constexpr console_tables_placement UNPLACED = {.console_bytes = 1, .offset = 1, .bytes = 1};

// Region 0 is mapped whole, holding exactly the reserved bytes or more, and the tables lie at the usable region's start
TEST(nvidia_bar1_page_tables, rtx_3080_tables_map_region_0_whole_and_lie_at_the_start_of_the_usable_region) {
    static_configuration configuration = rtx_3080_configuration();
    console_tables_placement placement = UNPLACED;

    ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement), OK);
    EXPECT_EQ(placement.console_bytes, RTX_3080_CONSOLE_BYTES);
    EXPECT_EQ(placement.offset, TABLES_OFFSET);
    EXPECT_EQ(placement.bytes, RTX_3080_TABLES_BYTES);

    placement = UNPLACED;
    ASSERT_EQ(
        place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES - TABLE_SPAN, RTX_3080_BAR1_BYTES, placement),
        OK
    );
    EXPECT_EQ(placement.console_bytes, RTX_3080_CONSOLE_BYTES);
}

// Region 0 ending 4 KiB before BAR1 does is mapped, and region 0 filling BAR1 is refused
TEST(nvidia_bar1_page_tables, region_0_is_refused_unless_it_ends_before_bar1_does) {
    static_configuration configuration = rtx_3080_configuration();
    console_tables_placement placement = UNPLACED;
    uint64_t bar1_bytes = RTX_3080_CONSOLE_BYTES + PAGE_BYTES;

    ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, bar1_bytes, placement), OK);
    EXPECT_EQ(placement.console_bytes, RTX_3080_CONSOLE_BYTES);

    placement = UNPLACED;
    bar1_bytes = RTX_3080_CONSOLE_BYTES;
    EXPECT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, bar1_bytes, placement), ERR_BAD_STATE);
    EXPECT_EQ(placement.offset, UNPLACED.offset);
}

// Region 0 starting past 0, smaller than the bytes reserved for the console, or ending inside a 4 KiB page is refused
TEST(nvidia_bar1_page_tables, region_0_is_refused_unless_it_starts_at_0_and_holds_the_reserved_bytes_in_whole_pages) {
    static_configuration configuration = rtx_3080_configuration();
    console_tables_placement placement = UNPLACED;

    configuration.regions[0].offset = PAGE_BYTES;
    EXPECT_EQ(
        place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES - TABLE_SPAN, RTX_3080_BAR1_BYTES, placement),
        ERR_BAD_STATE
    );
    configuration.regions[0].offset = 0;
    EXPECT_EQ(
        place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES + PAGE_BYTES, RTX_3080_BAR1_BYTES, placement),
        ERR_BAD_STATE
    );
    configuration.regions[0].size = RTX_3080_CONSOLE_BYTES + PAGE_BYTES / 2;
    EXPECT_EQ(
        place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement),
        ERR_BAD_STATE
    );
    EXPECT_EQ(placement.offset, UNPLACED.offset);
}

// A usable region too small for the tables, a protected region, a reserved region and a usable region 0 are skipped,
// and the tables start at the first 4 KiB boundary of the next usable region, whose room past it fits them exactly
TEST(nvidia_bar1_page_tables, tables_skip_regions_they_may_not_use_and_start_at_a_4_kib_boundary) {
    static_configuration configuration = {};
    configuration.regions[0] = {.offset = 0, .size = RTX_3080_CONSOLE_BYTES};
    configuration.regions[1] = {.offset = RTX_3080_CONSOLE_BYTES, .size = RTX_3080_TABLES_BYTES - PAGE_BYTES};
    configuration.regions[2] = {.offset = 0x1700000, .size = 0x100000, .is_protected = true};
    configuration.regions[3] = {.offset = 0x1800000, .size = 0x100000, .is_reserved = true};
    configuration.regions[4] = {.offset = 0x1900100, .size = RTX_3080_TABLES_BYTES + PAGE_BYTES - 0x100};
    configuration.region_count = 5;
    configuration.bar1_page_directory_offset = RTX_3080_BAR1_ROOT;
    console_tables_placement placement = UNPLACED;

    ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement), OK);
    EXPECT_EQ(placement.offset, static_cast<uint64_t>(0x1901000));
}

// A usable region whose start rounded up to 4 KiB wraps to 0 and one the tables fill up to 2^64, so their end wraps to
// 0, are skipped, and the tables lie at the start of the card's usable region listed after them
TEST(nvidia_bar1_page_tables, tables_skip_usable_regions_in_the_last_4_kib_below_2_64_or_ending_there) {
    static_configuration configuration = rtx_3080_configuration();
    configuration.regions[4] = configuration.regions[2];
    configuration.regions[2] = {.offset = 0xFFFFFFFFFFFFF001, .size = 0x10};
    configuration.regions[3] = {.offset = 0 - RTX_3080_TABLES_BYTES, .size = RTX_3080_TABLES_BYTES};
    configuration.region_count = 5;
    console_tables_placement placement = UNPLACED;

    ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement), OK);
    EXPECT_EQ(placement.offset, TABLES_OFFSET);
}

// The tables lie in region 1 when it is the first usable region they fit
TEST(nvidia_bar1_page_tables, tables_lie_in_region_1_when_it_is_the_first_usable_region_they_fit) {
    static_configuration configuration = rtx_3080_configuration();
    configuration.regions[1].is_reserved = false;
    console_tables_placement placement = UNPLACED;

    ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement), OK);
    EXPECT_EQ(placement.offset, RTX_3080_CONSOLE_BYTES);
}

// A usable region the tables fit, listed just past the count of regions, is never used, so counted regions that all
// fail are refused
TEST(nvidia_bar1_page_tables, a_fitting_region_past_the_count_of_regions_is_never_used) {
    static_configuration configuration = rtx_3080_configuration();
    configuration.regions[2].is_protected = true;
    configuration.regions[3] = {.offset = RTX_3080_BAR1_ROOT + PAGE_BYTES, .size = 0x100000};
    console_tables_placement placement = UNPLACED;

    EXPECT_EQ(
        place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement),
        ERR_BAD_STATE
    );
    EXPECT_EQ(placement.offset, UNPLACED.offset);
}

// When the root page directory's first entry would lie in the tables' first or last 8 bytes, the tables are placed in
// the next usable region, and with a root just before or just after them they are placed at the usable region's start
TEST(nvidia_bar1_page_tables, tables_never_hold_the_first_entry_of_bar1s_root) {
    static_configuration configuration = rtx_3080_configuration();
    configuration.regions[3] = {.offset = RTX_3080_BAR1_ROOT + PAGE_BYTES, .size = 0x100000};
    configuration.region_count = 4;
    uint64_t holding_roots[] = {TABLES_OFFSET, TABLES_OFFSET + RTX_3080_TABLES_BYTES - 8};
    uint64_t clear_roots[] = {TABLES_OFFSET - 8, TABLES_OFFSET + RTX_3080_TABLES_BYTES};
    console_tables_placement placement = UNPLACED;

    for (uint64_t root : holding_roots) {
        configuration.bar1_page_directory_offset = root;
        ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement), OK);
        EXPECT_EQ(placement.offset, RTX_3080_BAR1_ROOT + PAGE_BYTES);
    }

    for (uint64_t root : clear_roots) {
        configuration.bar1_page_directory_offset = root;
        ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement), OK);
        EXPECT_EQ(placement.offset, TABLES_OFFSET);
    }
}

// Tables ending at 128 GiB are placed, and tables 4 KiB higher, which would end past it, are refused
TEST(nvidia_bar1_page_tables, tables_end_by_128_gib) {
    static_configuration configuration = rtx_3080_configuration();
    uint64_t top_offset = ADDRESS_FIELD_REACH - RTX_3080_TABLES_BYTES;
    configuration.regions[2] = {.offset = top_offset, .size = 0x100000};
    console_tables_placement placement = UNPLACED;

    ASSERT_EQ(place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement), OK);
    EXPECT_EQ(placement.offset, top_offset);

    placement = UNPLACED;
    configuration.regions[2].offset = top_offset + PAGE_BYTES;
    EXPECT_EQ(
        place_console_page_tables(configuration, RTX_3080_CONSOLE_BYTES, RTX_3080_BAR1_BYTES, placement),
        ERR_BAD_STATE
    );
    EXPECT_EQ(placement.offset, UNPLACED.offset);
}
