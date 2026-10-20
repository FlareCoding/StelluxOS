// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_PAGE_TABLES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_PAGE_TABLES_H

#include "common/types.h"

namespace drivers::nvidia {

/*
 * BAR1 is the CPU's window into the card's video memory. Once the GSP, the
 * GPU's system processor, runs its firmware, BAR1 reaches video memory through
 * page tables the firmware binds BAR1 to. When an offset into BAR1 lies in a 4
 * KiB page, the card translates the offset by reading one entry from each of
 * five levels in turn: the root page directory the firmware places, then the
 * directories of levels 2, 1 and 0, then a page table, whose entry maps the
 * page. NVIDIA's diagram of the format numbers the root page directory 3 and
 * each directory after it one less. Each directory entry names the next
 * level's table by the table's offset in video memory. The card caches the
 * translations it reads, so a change to BAR1's tables takes effect only once
 * BAR1's cached translations are invalidated, as NVIDIA's driver does after it
 * replaces its own root page directory for BAR1 with the firmware's.
 *
 * A boot console is a boot framebuffer that may still be drawn in. The
 * console's tables map the first bytes of BAR1 onto the same offsets of video
 * memory, so a boot console lying at the start of both stays reachable at the
 * start of BAR1 while the firmware runs. They take effect once they lie in
 * video memory, entry 0 of the firmware's root page directory names their
 * level 2 directory and BAR1's cached translations are invalidated, in that
 * order. Since entry 0 covers far more than BAR1, the tables then cover all of
 * BAR1. An entry is sparse when it names nothing and an access through it does
 * not fault. Every entry the console does not use is sparse, so an access to
 * BAR1 past the console does not fault, as NVIDIA's driver makes sparse every
 * part of BAR1 it does not map.
 *
 *   root page directory   the firmware's, entry 0 covers 128 TiB
 *   level 2 directory     entry 0 covers 256 GiB
 *   level 1 directory     entry 0 covers 512 MiB
 *   level 0 directory     entry k covers 2 MiB, in 16 bytes
 *   page table k          entry j maps 4 KiB
 *
 * The tables must lie in video memory, since every directory entry names its
 * table there. NVIDIA's driver keeps BAR1's directories and page tables in
 * video memory by default and notes that a BAR's directories and page tables
 * in system memory can deadlock PCIe transactions. `write_console_page_tables`
 * builds a copy of the tables for its caller to write into video memory, one
 * block of 4 KiB tables in the diagram's order: the directories of levels 2, 1
 * and 0, then one page table for each 2 MiB of the console. Each page table
 * maps 4 KiB pages, the size NVIDIA's driver maps the console with. A page
 * table of larger pages maps 64 KiB or 128 KiB pages, whichever size BAR1 is
 * set up with, and the layout of a page table of 4 KiB pages does not depend
 * on that size, which this driver does not read.
 */

/**
 * @brief How many bytes the console's tables take for a console of
 * `console_bytes` bytes at the start of BAR1: 4 KiB for each of the three
 * directories and 4 KiB for each page table, one page table for each 2 MiB of
 * the console or part of it. The call never blocks.
 * @param console_bytes The console's size in bytes.
 * @return The bytes, a multiple of 4 KiB. It is 0 when `console_bytes` is 0,
 * leaving nothing to map, when `console_bytes` is not a multiple of 4 KiB,
 * since the page holding the console's last bytes would map bytes past the
 * console, or when `console_bytes` exceeds the 512 MiB one level 0 directory
 * covers.
 */
uint64_t console_page_table_bytes(uint64_t console_bytes);

/**
 * @brief Write the console's tables into `tables`, mapping the first
 * `console_bytes` bytes of BAR1 onto the same offsets of video memory in 4 KiB
 * pages. Every entry the console uses names a table or a page in video memory:
 * a directory entry the next level's table, a page table entry its page. Each
 * holds the offset of what it names in its address field, in 4 KiB units, and
 * video memory in its aperture field, which names the memory that offset lies
 * in. A page table entry also has its valid bit set. Every other field is 0,
 * the volatile bit included. In an entry naming memory, that bit asks the card
 * not to cache the memory, and NVIDIA's driver leaves it 0 in its entries for
 * video memory. In an entry naming nothing, it marks the entry sparse. A page
 * table entry's kind field gives the layout of its page's memory, and kind 0
 * is NVIDIA's kind for memory laid out row by row as a framebuffer is. Every
 * entry the console does not use holds NVIDIA's sparse value instead, which
 * names nothing and has its volatile bit set. The call never blocks.
 * @param tables The CPU's copy of the tables, which must hold
 * `console_page_table_bytes(console_bytes)` bytes. The call writes all of them
 * unless it returns 0.
 * @param tables_offset Where in video memory the caller must write the copy.
 * Since the console takes the first `console_bytes` bytes of video memory, the
 * tables must start at `console_bytes` or past it.
 * @param console_bytes The console's size in bytes.
 * @return The value entry 0 of the firmware's root page directory must hold
 * for BAR1's first `console_bytes` bytes to map through the tables, the
 * directory entry naming the level 2 directory at `tables_offset`. It is 0
 * when `console_page_table_bytes` refuses `console_bytes`, when
 * `tables_offset` is not a multiple of 4 KiB (an entry's address field counts
 * 4 KiB units), when the tables would start inside the console, or when the
 * tables would end past 128 GiB (the field holds only offsets below 128 GiB).
 * `tables` is then left as it is. No directory entry naming video memory is 0.
 */
uint64_t write_console_page_tables(uint64_t* tables, uint64_t tables_offset, uint64_t console_bytes);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_PAGE_TABLES_H
