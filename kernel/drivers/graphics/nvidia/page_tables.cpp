// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/page_tables.h"

namespace drivers::nvidia {

// The entries the builder writes hold the offset of what they name in 4 KiB units in their address field, from bit 8
constexpr uint64_t ADDRESS_UNIT_SHIFT  = 12;
constexpr uint64_t ADDRESS_UNIT_BYTES  = 1ULL << ADDRESS_UNIT_SHIFT;
constexpr uint64_t ADDRESS_FIELD_SHIFT = 8;

// Video memory is aperture 1 (bits 2:1) in a directory entry, where 0 names nothing, and 0 in a page table entry. A
// directory entry naming a table keeps bit 0 clear, since at level 0 or 1 it would map a 2 MiB or 512 MiB page instead
constexpr uint64_t DIRECTORY_IN_VIDEO_MEMORY = 1ULL << 1;
constexpr uint64_t PAGE_VALID                = 1;

// The sparse value sets the volatile bit, bit 3, in an entry naming nothing
constexpr uint64_t SPARSE_ENTRY = 1ULL << 3;

// A level 0 entry's first word names a page table of larger pages and its second a page table of 4 KiB pages. Only the
// first word of a sparse level 0 entry holds the sparse value
constexpr uint64_t LARGE_PAGES_WORD = 0;
constexpr uint64_t SMALL_PAGES_WORD = 1;
constexpr uint64_t DUAL_ENTRY_WORDS = 2;

namespace {

struct level_layout {
    uint32_t index_shift;
    uint64_t entries;
    uint64_t entry_words;
    bool names_tables;
};

} // namespace

static level_layout layout_of(page_level level) {
    switch (level) {
        case page_level::directory_2:
            return {.index_shift = 38, .entries = 512, .entry_words = 1, .names_tables = true};
        case page_level::directory_1:
            return {.index_shift = 29, .entries = 512, .entry_words = 1, .names_tables = true};
        case page_level::directory_0:
            return {.index_shift = 21, .entries = 256, .entry_words = DUAL_ENTRY_WORDS, .names_tables = true};
        case page_level::page_table:
            break;
    }

    return {.index_shift = 12, .entries = 512, .entry_words = 1, .names_tables = false};
}

static bool is_nameable(uint64_t vram_offset) {
    return vram_offset % ADDRESS_UNIT_BYTES == 0 && vram_offset < ENTRY_REACH_BYTES;
}

static uint64_t address_field(uint64_t vram_offset) {
    return (vram_offset >> ADDRESS_UNIT_SHIFT) << ADDRESS_FIELD_SHIFT;
}

uint64_t entry_span(page_level level) {
    return 1ULL << layout_of(level).index_shift;
}

uint64_t table_entry_offset(page_level level, uint64_t address) {
    level_layout layout = layout_of(level);
    uint64_t index = (address >> layout.index_shift) % layout.entries;
    return index * layout.entry_words * sizeof(uint64_t);
}

uint64_t directory_entry(uint64_t table_offset) {
    if (!is_nameable(table_offset)) {
        return 0;
    }

    return address_field(table_offset) | DIRECTORY_IN_VIDEO_MEMORY;
}

uint64_t page_entry(uint64_t vram_offset) {
    if (!is_nameable(vram_offset)) {
        return 0;
    }

    return address_field(vram_offset) | PAGE_VALID;
}

uint64_t sparse_page_entry() {
    return SPARSE_ENTRY;
}

void write_sparse_table(uint64_t* table, page_level level) {
    level_layout layout = layout_of(level);
    for (uint64_t i = 0; i < layout.entries; i++) {
        uint64_t* entry = table + i * layout.entry_words;
        entry[0] = SPARSE_ENTRY;

        if (layout.entry_words == DUAL_ENTRY_WORDS) {
            entry[SMALL_PAGES_WORD] = 0;
        }
    }
}

void write_directory_entry(uint64_t* directory, page_level level, uint64_t address, uint64_t table_offset) {
    level_layout layout = layout_of(level);
    uint64_t named_entry = directory_entry(table_offset);
    if (!layout.names_tables || named_entry == 0) {
        return;
    }

    uint64_t* entry = directory + table_entry_offset(level, address) / sizeof(uint64_t);
    if (layout.entry_words == DUAL_ENTRY_WORDS) {
        entry[LARGE_PAGES_WORD] = 0;
        entry[SMALL_PAGES_WORD] = named_entry;
        return;
    }

    entry[0] = named_entry;
}

} // namespace drivers::nvidia
