// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/page_tables.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_page_tables);

using drivers::nvidia::ENTRY_REACH_BYTES;
using drivers::nvidia::TABLE_BYTES;
using drivers::nvidia::directory_entry;
using drivers::nvidia::entry_span;
using drivers::nvidia::page_entry;
using drivers::nvidia::page_level;
using drivers::nvidia::sparse_page_entry;
using drivers::nvidia::table_entry_offset;
using drivers::nvidia::write_directory_entry;
using drivers::nvidia::write_sparse_table;

// A table's 512 words, and NVIDIA's sparse value, which sets the volatile bit, bit 3, in an entry naming nothing
static constexpr uint64_t TABLE_WORDS  = 512;
static constexpr uint64_t SPARSE_ENTRY = 0x8;

// The 128 GiB an entry's 25-bit address field of 4 KiB units reaches
static constexpr uint64_t ADDRESS_FIELD_REACH = 1ULL << 37;

// A value no entry takes, filling each copy before a call, and the guard table after the copy, which every call must
// leave holding it
static constexpr uint64_t UNWRITTEN_WORD = 0xA5A5A5A5A5A5A5A5;

// A copy of one table followed by the guard table, every word holding UNWRITTEN_WORD
static uint64_t* alloc_table() {
    uint64_t words = 2 * TABLE_WORDS;
    uint64_t* copy = static_cast<uint64_t*>(heap::ualloc(words * sizeof(uint64_t)));
    if (copy == nullptr) {
        return nullptr;
    }

    for (uint64_t i = 0; i < words; i++) {
        copy[i] = UNWRITTEN_WORD;
    }

    return copy;
}

// How many words of the guard table after `copy` hold a value other than UNWRITTEN_WORD
static uint64_t written_guard_words(const uint64_t* copy) {
    uint64_t written = 0;
    for (uint64_t i = TABLE_WORDS; i < 2 * TABLE_WORDS; i++) {
        written += copy[i] != UNWRITTEN_WORD;
    }

    return written;
}

TEST(nvidia_page_tables, an_entry_covers_256_gib_512_mib_2_mib_or_4_kib_by_level) {
    EXPECT_EQ(entry_span(page_level::directory_2), 1ULL << 38);
    EXPECT_EQ(entry_span(page_level::directory_1), 1ULL << 29);
    EXPECT_EQ(entry_span(page_level::directory_0), 1ULL << 21);
    EXPECT_EQ(entry_span(page_level::page_table), 1ULL << 12);
}

TEST(nvidia_page_tables, a_table_takes_4_kib_and_an_entry_reaches_128_gib) {
    EXPECT_EQ(TABLE_BYTES, static_cast<uint64_t>(0x1000));
    EXPECT_EQ(ENTRY_REACH_BYTES, ADDRESS_FIELD_REACH);
}

// An address whose index is 3 at level 2, 5 at level 1, 7 at level 0 and 9 in its page table, with bits below the page
// and above level 2 that no index of these levels reads
static constexpr uint64_t INDEXED_ADDRESS = 1ULL << 47 | 3ULL << 38 | 5ULL << 29 | 7ULL << 21 | 9ULL << 12 | 0x123;

// An address whose index is the last entry at levels 2, 1 and 0 and in its page table
static constexpr uint64_t LAST_ENTRIES_ADDRESS = (1ULL << 47) - 1;

TEST(nvidia_page_tables, an_entrys_offset_comes_from_its_levels_bits_of_the_address) {
    EXPECT_EQ(table_entry_offset(page_level::directory_2, INDEXED_ADDRESS), static_cast<uint64_t>(3 * 8));
    EXPECT_EQ(table_entry_offset(page_level::directory_1, INDEXED_ADDRESS), static_cast<uint64_t>(5 * 8));
    EXPECT_EQ(table_entry_offset(page_level::directory_0, INDEXED_ADDRESS), static_cast<uint64_t>(7 * 16));
    EXPECT_EQ(table_entry_offset(page_level::page_table, INDEXED_ADDRESS), static_cast<uint64_t>(9 * 8));

    EXPECT_EQ(table_entry_offset(page_level::directory_2, LAST_ENTRIES_ADDRESS), static_cast<uint64_t>(0xFF8));
    EXPECT_EQ(table_entry_offset(page_level::directory_1, LAST_ENTRIES_ADDRESS), static_cast<uint64_t>(0xFF8));
    EXPECT_EQ(table_entry_offset(page_level::directory_0, LAST_ENTRIES_ADDRESS), static_cast<uint64_t>(0xFF0));
    EXPECT_EQ(table_entry_offset(page_level::page_table, LAST_ENTRIES_ADDRESS), static_cast<uint64_t>(0xFF8));
}

// The start of the first region the RTX 3080's GSP firmware reports as usable with the boot console on the card, where
// BAR1's tables lie, and the last 4 KiB below the entries' 128 GiB reach, whose entry sets bit 32
static constexpr uint64_t RTX_3080_TABLES_OFFSET = 0x3F80000;
static constexpr uint64_t LAST_NAMEABLE_OFFSET   = ADDRESS_FIELD_REACH - 0x1000;

TEST(nvidia_page_tables, a_directory_entry_names_a_table_below_128_gib_and_refuses_the_rest) {
    EXPECT_EQ(directory_entry(RTX_3080_TABLES_OFFSET), static_cast<uint64_t>(0x3F8002));
    EXPECT_EQ(directory_entry(LAST_NAMEABLE_OFFSET), static_cast<uint64_t>(0x1FFFFFF02));
    EXPECT_EQ(directory_entry(0), static_cast<uint64_t>(0x2));
    EXPECT_EQ(directory_entry(ADDRESS_FIELD_REACH), static_cast<uint64_t>(0));
    EXPECT_EQ(directory_entry(RTX_3080_TABLES_OFFSET + 0x800), static_cast<uint64_t>(0));
}

TEST(nvidia_page_tables, a_page_entry_maps_a_page_below_128_gib_and_refuses_the_rest) {
    EXPECT_EQ(page_entry(0), static_cast<uint64_t>(0x1));
    EXPECT_EQ(page_entry(0x1235000), static_cast<uint64_t>(0x123501));
    EXPECT_EQ(page_entry(LAST_NAMEABLE_OFFSET), static_cast<uint64_t>(0x1FFFFFF01));
    EXPECT_EQ(page_entry(ADDRESS_FIELD_REACH), static_cast<uint64_t>(0));
    EXPECT_EQ(page_entry(0x1008), static_cast<uint64_t>(0));
}

// A sparse directory of level 2 or 1 and a sparse page table hold the sparse value in every word, and a sparse level 0
// directory holds it in the first half of each of its 256 entries, with 0 in the second
TEST(nvidia_page_tables, a_sparse_table_holds_the_sparse_value_in_every_entry_of_its_level) {
    EXPECT_EQ(sparse_page_entry(), SPARSE_ENTRY);

    static constexpr page_level WORD_ENTRY_LEVELS[] = {
        page_level::directory_2,
        page_level::directory_1,
        page_level::page_table,
    };

    for (page_level level : WORD_ENTRY_LEVELS) {
        uint64_t* table = alloc_table();
        ASSERT_NOT_NULL(table);

        write_sparse_table(table, level);

        uint64_t wrong_words = 0;
        for (uint64_t i = 0; i < TABLE_WORDS; i++) {
            wrong_words += table[i] != SPARSE_ENTRY;
        }

        EXPECT_EQ(wrong_words, static_cast<uint64_t>(0));
        EXPECT_EQ(written_guard_words(table), static_cast<uint64_t>(0));
        heap::ufree(table);
    }

    uint64_t* level_0 = alloc_table();
    ASSERT_NOT_NULL(level_0);

    write_sparse_table(level_0, page_level::directory_0);

    uint64_t wrong_words = 0;
    for (uint64_t i = 0; i < TABLE_WORDS; i += 2) {
        wrong_words += level_0[i] != SPARSE_ENTRY;
        wrong_words += level_0[i + 1] != 0;
    }

    EXPECT_EQ(wrong_words, static_cast<uint64_t>(0));
    EXPECT_EQ(written_guard_words(level_0), static_cast<uint64_t>(0));
    heap::ufree(level_0);
}

// Naming a table changes only the entry translating the address, one word at level 1 and both words at level 0, whose
// first word then names no table of larger pages
TEST(nvidia_page_tables, a_named_table_takes_the_entry_translating_its_address_alone) {
    uint64_t* level_1 = alloc_table();
    ASSERT_NOT_NULL(level_1);

    write_directory_entry(level_1, page_level::directory_1, INDEXED_ADDRESS, RTX_3080_TABLES_OFFSET);

    uint64_t wrong_words = 0;
    for (uint64_t i = 0; i < TABLE_WORDS; i++) {
        wrong_words += level_1[i] != (i == 5 ? static_cast<uint64_t>(0x3F8002) : UNWRITTEN_WORD);
    }

    EXPECT_EQ(wrong_words, static_cast<uint64_t>(0));
    EXPECT_EQ(written_guard_words(level_1), static_cast<uint64_t>(0));
    heap::ufree(level_1);

    uint64_t* level_0 = alloc_table();
    ASSERT_NOT_NULL(level_0);

    write_directory_entry(level_0, page_level::directory_0, INDEXED_ADDRESS, LAST_NAMEABLE_OFFSET);

    wrong_words = 0;
    for (uint64_t i = 0; i < TABLE_WORDS; i += 2) {
        bool is_named = i == 2 * 7;
        wrong_words += level_0[i] != (is_named ? 0 : UNWRITTEN_WORD);
        wrong_words += level_0[i + 1] != (is_named ? static_cast<uint64_t>(0x1FFFFFF02) : UNWRITTEN_WORD);
    }

    EXPECT_EQ(wrong_words, static_cast<uint64_t>(0));
    EXPECT_EQ(written_guard_words(level_0), static_cast<uint64_t>(0));
    heap::ufree(level_0);
}

// The page table's level, or an offset off a 4 KiB multiple or at the reach, leaves the whole copy as it was
TEST(nvidia_page_tables, a_refused_name_leaves_the_whole_copy_unwritten) {
    uint64_t* table = alloc_table();
    ASSERT_NOT_NULL(table);

    write_directory_entry(table, page_level::page_table, INDEXED_ADDRESS, RTX_3080_TABLES_OFFSET);
    write_directory_entry(table, page_level::directory_1, INDEXED_ADDRESS, RTX_3080_TABLES_OFFSET + 0x800);
    write_directory_entry(table, page_level::directory_0, INDEXED_ADDRESS, ADDRESS_FIELD_REACH);

    uint64_t wrong_words = 0;
    for (uint64_t i = 0; i < 2 * TABLE_WORDS; i++) {
        wrong_words += table[i] != UNWRITTEN_WORD;
    }

    EXPECT_EQ(wrong_words, static_cast<uint64_t>(0));
    heap::ufree(table);
}
