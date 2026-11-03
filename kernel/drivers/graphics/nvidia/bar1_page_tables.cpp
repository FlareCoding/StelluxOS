// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/bar1_page_tables.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/page_tables.h"
#include "common/logging.h"

namespace drivers::nvidia {

// BAR1's block holds the directories of levels 2, 1 and 0 ahead of its page tables
constexpr uint64_t DIRECTORY_COUNT = 3;
constexpr uint64_t TABLE_WORDS     = TABLE_BYTES / sizeof(uint64_t);

// The most bytes a window holds, what the one level 0 directory covers
static uint64_t max_window_bytes() {
    return entry_span(page_level::directory_1);
}

// How many page tables a window of `window_bytes` bytes takes, one for each 2 MiB of it or part of it
static uint64_t count_page_tables(uint64_t window_bytes) {
    uint64_t page_table_span = entry_span(page_level::directory_0);
    return (window_bytes + page_table_span - 1) / page_table_span;
}

// Which word of the page tables holds the entry for BAR1's page at `bar1_offset`
static uint64_t page_entry_word(uint64_t bar1_offset) {
    uint64_t page_table = bar1_offset / entry_span(page_level::directory_0);
    uint64_t offset_in_table = table_entry_offset(page_level::page_table, bar1_offset);
    return page_table * TABLE_WORDS + offset_in_table / sizeof(uint64_t);
}

uint64_t bar1_window_bytes(uint64_t bar1_bytes) {
    if (bar1_bytes <= SMALL_PAGE_BYTES) {
        return 0;
    }

    uint64_t mappable_bytes = (bar1_bytes - SMALL_PAGE_BYTES) / SMALL_PAGE_BYTES * SMALL_PAGE_BYTES;
    uint64_t max_bytes = max_window_bytes();
    return mappable_bytes < max_bytes ? mappable_bytes : max_bytes;
}

uint64_t bar1_page_table_bytes(uint64_t window_bytes) {
    bool is_empty = window_bytes == 0;
    bool is_whole_pages = window_bytes % SMALL_PAGE_BYTES == 0;
    bool fits_level_0 = window_bytes <= max_window_bytes();
    if (is_empty || !is_whole_pages || !fits_level_0) {
        return 0;
    }

    return (DIRECTORY_COUNT + count_page_tables(window_bytes)) * TABLE_BYTES;
}

uint64_t write_bar1_page_tables(
    uint64_t* tables,
    uint64_t tables_offset,
    uint64_t window_bytes,
    uint64_t console_bytes
) {
    uint64_t table_bytes = bar1_page_table_bytes(window_bytes);
    bool is_refused_window = table_bytes == 0;
    bool is_whole_console_pages = console_bytes % SMALL_PAGE_BYTES == 0;
    bool is_console_in_window = console_bytes <= window_bytes;
    if (is_refused_window || !is_whole_console_pages || !is_console_in_window) {
        return 0;
    }

    bool is_table_aligned = tables_offset % TABLE_BYTES == 0;
    bool is_past_console = tables_offset >= console_bytes;
    bool is_nameable = lies_inside(tables_offset, table_bytes, ENTRY_REACH_BYTES);
    if (!is_table_aligned || !is_past_console || !is_nameable) {
        return 0;
    }

    uint64_t* level_2 = tables;
    uint64_t* level_1 = level_2 + TABLE_WORDS;
    uint64_t* level_0 = level_1 + TABLE_WORDS;
    uint64_t* page_tables = level_0 + TABLE_WORDS;

    uint64_t level_1_offset = tables_offset + TABLE_BYTES;
    uint64_t level_0_offset = level_1_offset + TABLE_BYTES;
    uint64_t page_tables_offset = level_0_offset + TABLE_BYTES;
    uint64_t page_table_count = count_page_tables(window_bytes);
    uint64_t page_table_span = entry_span(page_level::directory_0);

    write_sparse_table(level_2, page_level::directory_2);
    write_directory_entry(level_2, page_level::directory_2, 0, level_1_offset);

    write_sparse_table(level_1, page_level::directory_1);
    write_directory_entry(level_1, page_level::directory_1, 0, level_0_offset);

    write_sparse_table(level_0, page_level::directory_0);
    for (uint64_t k = 0; k < page_table_count; k++) {
        uint64_t page_table_offset = page_tables_offset + k * TABLE_BYTES;
        write_directory_entry(level_0, page_level::directory_0, k * page_table_span, page_table_offset);
        write_sparse_table(page_tables + k * TABLE_WORDS, page_level::page_table);
    }

    for (uint64_t bar1_offset = 0; bar1_offset < console_bytes; bar1_offset += SMALL_PAGE_BYTES) {
        page_tables[page_entry_word(bar1_offset)] = page_entry(bar1_offset);
    }

    return directory_entry(tables_offset);
}

uint64_t bar1_page_entry_offset(uint64_t tables_offset, uint64_t bar1_offset) {
    uint64_t page_tables_offset = tables_offset + DIRECTORY_COUNT * TABLE_BYTES;
    return page_tables_offset + page_entry_word(bar1_offset) * sizeof(uint64_t);
}

int32_t check_console_region(
    const static_configuration& configuration,
    uint64_t reserved_console_bytes,
    uint64_t bar1_bytes,
    uint64_t& console_bytes_out
) {
    const video_memory_region& console_region = configuration.regions[0];
    if (!console_region.is_reserved) {
        log::error(
            "nvidia: video memory region 0 (0x%lX to 0x%lX), the boot console's, is usable rather than reserved, so a "
            "video memory allocation could overwrite the console",
            console_region.offset,
            console_region.offset + console_region.size - 1
        );
        return ERR_BAD_STATE;
    }

    uint64_t console_bytes = console_region.size;
    bool starts_at_zero = console_region.offset == 0;
    bool holds_reserved_bytes = console_bytes >= reserved_console_bytes;
    bool is_whole_pages = console_bytes % SMALL_PAGE_BYTES == 0;
    if (!starts_at_zero || !holds_reserved_bytes || !is_whole_pages) {
        log::error(
            "nvidia: video memory region 0 holds 0x%lX bytes at 0x%lX, where BAR1's tables need it to start at 0, hold "
            "the 0x%lX bytes reserved for the boot console and hold whole 4 KiB pages",
            console_bytes,
            console_region.offset,
            reserved_console_bytes
        );
        return ERR_BAD_STATE;
    }

    uint64_t window_bytes = bar1_window_bytes(bar1_bytes);
    if (console_bytes > window_bytes) {
        log::error(
            "nvidia: the console's 0x%lX bytes do not fit BAR1's window of 0x%lX bytes, which leaves out BAR1's last "
            "4 KiB and holds at most 512 MiB",
            console_bytes,
            window_bytes
        );
        return ERR_BAD_STATE;
    }

    console_bytes_out = console_bytes;
    return OK;
}

} // namespace drivers::nvidia
