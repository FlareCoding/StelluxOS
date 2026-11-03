// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_PAGE_TABLES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_PAGE_TABLES_H

#include "common/types.h"

/*
 * The page table builder writes the GPU's page tables in the version 2 layout
 * of its memory management unit, the layout of every address space the driver
 * builds tables for. The GPU translates an address in a 4 KiB page through
 * five levels of tables: a root page directory, the directories of levels 2, 1
 * and 0, then a page table whose entry maps the page. A directory entry names
 * the next level's table by its offset in video memory. Every table the
 * builder writes takes 4 KiB. An entry is sparse when it names nothing and an
 * access through it does not fault.
 *
 * The builder writes entries into the caller's copy of a table and keeps no
 * state. The caller must place each table, write the copies into video memory
 * and invalidate the GPU's cached translations. No call blocks.
 */

namespace drivers::nvidia {

// The levels the builder writes tables of, below the root page directory
enum class page_level : uint8_t {
    directory_2, // an entry covers 256 GiB
    directory_1, // an entry covers 512 MiB
    directory_0, // an entry covers 2 MiB and takes 16 bytes
    page_table,  // an entry maps one 4 KiB page
};

// The size of every table the builder writes, and the multiple its offset must be
constexpr uint64_t TABLE_BYTES = 0x1000;

// An entry names only offsets of video memory below 128 GiB, the reach of its address field
constexpr uint64_t ENTRY_REACH_BYTES = 1ULL << 37;

// The bytes of addresses one entry of `level` covers
uint64_t entry_span(page_level level);

// Where the entry translating `address` lies in its table of `level`, in bytes from the table's start. An address of
// BAR1's space is an offset into BAR1
uint64_t table_entry_offset(page_level level, uint64_t address);

// The directory entry naming the table at `table_offset` in video memory, or 0, which no such entry is, when
// `table_offset` is not a multiple of 4 KiB below `ENTRY_REACH_BYTES`
uint64_t directory_entry(uint64_t table_offset);

// The page table entry mapping the 4 KiB page at `vram_offset` in video memory with kind 0 (memory laid out row by
// row), or 0, which no such entry is, when `vram_offset` is not a multiple of 4 KiB below `ENTRY_REACH_BYTES`
uint64_t page_entry(uint64_t vram_offset);

// The sparse page table entry, which maps no page
uint64_t sparse_page_entry();

// Makes every entry of `table`, the caller's copy of a table of `level`, sparse
void write_sparse_table(uint64_t* table, page_level level);

// Makes the entry translating `address` in `directory`, the caller's copy of a directory of `level`, name the table at
// `table_offset`, at level 0 a page table of 4 KiB pages with no page table of larger pages. The other entries keep
// their values, and so does this one when `level` is the page table's or `directory_entry` refuses `table_offset`
void write_directory_entry(uint64_t* directory, page_level level, uint64_t address, uint64_t table_offset);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_PAGE_TABLES_H
