// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/bar1_page_tables.h"
#include "drivers/graphics/nvidia/nvidia.h"

namespace drivers::nvidia {

// A table holds 4 KiB of 8-byte words, one entry each but in level 0, whose entries take two, and a page table entry
// maps a 4 KiB page, so a page table maps 2 MiB
constexpr uint64_t TABLE_BYTES      = 0x1000;
constexpr uint64_t TABLE_WORDS      = TABLE_BYTES / sizeof(uint64_t);
constexpr uint64_t PAGE_BYTES       = 0x1000;
constexpr uint64_t TABLE_SPAN_BYTES = TABLE_WORDS * PAGE_BYTES;
constexpr uint64_t DIRECTORY_COUNT  = 3;

// A level 0 entry takes 16 bytes. Its first half would name a page table of 64 KiB or 128 KiB pages and names none in
// the console's tables. Its second half, laid out as a directory entry, names a page table of 4 KiB pages or nothing
constexpr uint64_t LEVEL_0_ENTRY_WORDS = 2;
constexpr uint64_t LARGE_PAGES_WORD    = 0;
constexpr uint64_t SMALL_PAGES_WORD    = 1;
constexpr uint64_t LEVEL_0_ENTRIES     = TABLE_WORDS / LEVEL_0_ENTRY_WORDS;
constexpr uint64_t MAX_CONSOLE_BYTES   = LEVEL_0_ENTRIES * TABLE_SPAN_BYTES;

// An entry holds the offset of what it names in 4 KiB units in a 25-bit address field from bit 8, so it names only
// offsets below 128 GiB
constexpr uint64_t ADDRESS_UNIT_SHIFT  = 12;
constexpr uint64_t ADDRESS_UNIT_BYTES  = 1ull << ADDRESS_UNIT_SHIFT;
constexpr uint64_t ADDRESS_FIELD_SHIFT = 8;
constexpr uint64_t ADDRESS_FIELD_BITS  = 25;
constexpr uint64_t ADDRESS_FIELD_REACH = 1ull << (ADDRESS_FIELD_BITS + ADDRESS_UNIT_SHIFT);

// An entry's aperture in bits 2:1 names the memory its offset lies in, video memory as 1 in a directory entry and as 0
// in a page table entry. A directory entry or a half of a level 0 entry whose aperture is 0 names nothing
constexpr uint64_t DIRECTORY_IN_VIDEO_MEMORY = 1ull << 1;

// Bit 0 marks a page table entry valid and is 0 in a directory entry, where it marks the entry as naming a table rather
// than mapping a page
constexpr uint64_t PAGE_VALID = 1;

// NVIDIA's sparse entry names nothing and sets the volatile bit, bit 3, so an access through it does not fault. Level 0
// entries set it in their first half alone, and only where their second half names no page table
constexpr uint64_t SPARSE_ENTRY = 1ull << 3;

// The directory entry naming the table at `offset` in video memory
static uint64_t directory_entry(uint64_t offset) {
    return (offset >> ADDRESS_UNIT_SHIFT) << ADDRESS_FIELD_SHIFT | DIRECTORY_IN_VIDEO_MEMORY;
}

// The page table entry mapping the page at `offset` in video memory
static uint64_t page_entry(uint64_t offset) {
    return (offset >> ADDRESS_UNIT_SHIFT) << ADDRESS_FIELD_SHIFT | PAGE_VALID;
}

// How many page tables map a console of `console_bytes` bytes, one for each 2 MiB of it or part of it
static uint64_t count_page_tables(uint64_t console_bytes) {
    return (console_bytes + TABLE_SPAN_BYTES - 1) / TABLE_SPAN_BYTES;
}

uint64_t console_page_table_bytes(uint64_t console_bytes) {
    bool is_empty = console_bytes == 0;
    bool is_whole_pages = console_bytes % PAGE_BYTES == 0;
    bool fits_level_0 = console_bytes <= MAX_CONSOLE_BYTES;
    if (is_empty || !is_whole_pages || !fits_level_0) {
        return 0;
    }

    return (DIRECTORY_COUNT + count_page_tables(console_bytes)) * TABLE_BYTES;
}

uint64_t write_console_page_tables(uint64_t* tables, uint64_t tables_offset, uint64_t console_bytes) {
    uint64_t table_bytes = console_page_table_bytes(console_bytes);
    bool is_refused_size = table_bytes == 0;
    bool is_unit_aligned = tables_offset % ADDRESS_UNIT_BYTES == 0;
    bool is_past_console = tables_offset >= console_bytes;
    bool is_nameable = lies_inside(tables_offset, table_bytes, ADDRESS_FIELD_REACH);
    if (is_refused_size || !is_unit_aligned || !is_past_console || !is_nameable) {
        return 0;
    }

    uint64_t* level_2 = tables;
    uint64_t* level_1 = level_2 + TABLE_WORDS;
    uint64_t* level_0 = level_1 + TABLE_WORDS;
    uint64_t* page_tables = level_0 + TABLE_WORDS;

    uint64_t level_1_offset = tables_offset + TABLE_BYTES;
    uint64_t level_0_offset = level_1_offset + TABLE_BYTES;
    uint64_t page_tables_offset = level_0_offset + TABLE_BYTES;
    uint64_t page_table_count = count_page_tables(console_bytes);
    uint64_t pages = console_bytes / PAGE_BYTES;

    level_2[0] = directory_entry(level_1_offset);
    level_1[0] = directory_entry(level_0_offset);

    for (uint64_t i = 1; i < TABLE_WORDS; i++) {
        level_2[i] = SPARSE_ENTRY;
        level_1[i] = SPARSE_ENTRY;
    }

    for (uint64_t k = 0; k < LEVEL_0_ENTRIES; k++) {
        uint64_t* entry = level_0 + k * LEVEL_0_ENTRY_WORDS;
        bool names_page_table = k < page_table_count;
        uint64_t page_table_offset = page_tables_offset + k * TABLE_BYTES;

        entry[LARGE_PAGES_WORD] = names_page_table ? 0 : SPARSE_ENTRY;
        entry[SMALL_PAGES_WORD] = names_page_table ? directory_entry(page_table_offset) : 0;
    }

    for (uint64_t page = 0; page < page_table_count * TABLE_WORDS; page++) {
        page_tables[page] = page < pages ? page_entry(page * PAGE_BYTES) : SPARSE_ENTRY;
    }

    return directory_entry(tables_offset);
}

} // namespace drivers::nvidia
