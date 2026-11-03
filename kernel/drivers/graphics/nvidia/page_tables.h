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
 * builder writes takes 4 KiB. An entry of 0 is invalid: it names nothing and
 * an access through it faults. An entry is sparse when it names nothing and an
 * access through it does not fault. A page table entry also gives its page's
 * aperture (the memory the page lies in) and kind (the layout of its memory,
 * kind 0 laying it out row by row).
 *
 * The builder writes entries into the caller's copy of a table and keeps no
 * state. The caller must place each table, write the copies into video memory
 * and invalidate the GPU's cached translations. No call blocks.
 */

namespace drivers::nvidia {

// The levels the builder writes tables of
enum class page_level : uint8_t {
    root,        // an entry covers 128 TiB, and the table's first 4 entries alone translate
    directory_2, // an entry covers 256 GiB
    directory_1, // an entry covers 512 MiB
    directory_0, // an entry covers 2 MiB and takes 16 bytes
    page_table,  // an entry maps one 4 KiB page
};

// The size of every table the builder writes, and the multiple its offset must be
constexpr uint64_t TABLE_BYTES = 0x1000;

// An entry naming video memory names only offsets below 128 GiB, the reach of its address field there
constexpr uint64_t ENTRY_REACH_BYTES = 1ULL << 37;

// A page table entry names only physical addresses of system memory below 2^58, the reach of its address field there
constexpr uint64_t SYSTEM_ENTRY_REACH_BYTES = 1ULL << 58;

// The bytes of GPU addresses that the root page directory's 4 entries translate, 512 TiB
constexpr uint64_t ADDRESS_SPACE_BYTES = 1ULL << 49;

// The last kind `page_entry` accepts. Kinds 0 to 6 need no compression, which the driver never sets up
constexpr uint8_t LAST_UNCOMPRESSED_KIND = 6;

// Where the page a page table entry maps lies
enum class page_aperture : uint8_t {
    video_memory,  // at an offset into the card's video memory
    system_memory, // at a physical address, reached coherently with the CPU's caches and never cached by the GPU
};

// How a page table entry maps its page, by default as BAR1 maps video memory
struct page_mapping {
    page_aperture aperture  = page_aperture::video_memory;
    uint8_t       kind      = 0;
    bool          read_only = false; // the GPU faults on a write through the entry
};

// The bytes of addresses one entry of `level` covers
uint64_t entry_span(page_level level);

// The bytes one entry of a table of `level` takes
uint64_t entry_bytes(page_level level);

// Where the entry translating `address` lies in its table of `level`, in bytes from the table's start. An address of
// BAR1's space is an offset into BAR1
uint64_t table_entry_offset(page_level level, uint64_t address);

// The directory entry naming the table at `table_offset` in video memory, or 0, which no such entry is, when
// `table_offset` is not a multiple of 4 KiB below `ENTRY_REACH_BYTES`
uint64_t directory_entry(uint64_t table_offset);

// The page table entry mapping the 4 KiB page at `address`, an offset into video memory or a physical address of system
// memory as `mapping` says. It is 0, which no such entry is, when `address` is not a multiple of 4 KiB below its
// aperture's reach or the kind is past `LAST_UNCOMPRESSED_KIND`
uint64_t page_entry(uint64_t address, const page_mapping& mapping = {});

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
