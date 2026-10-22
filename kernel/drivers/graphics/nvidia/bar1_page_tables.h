// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_PAGE_TABLES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_PAGE_TABLES_H

#include "drivers/graphics/nvidia/gsp_info.h"
#include "common/types.h"

namespace drivers::nvidia {

/*
 * BAR1 is where the CPU reaches the card's video memory. Once the GSP, the
 * GPU's system processor, runs its firmware, BAR1 reaches video memory through
 * page tables the firmware binds BAR1 to. When an offset into BAR1 lies in a 4
 * KiB page, the card translates the offset by reading one entry from each of
 * five levels in turn: the root page directory the firmware places, then the
 * directories of levels 2, 1 and 0, then a page table, whose entry maps the
 * page. NVIDIA's diagram of the format numbers the root page directory 3 and
 * each directory after it one less. Each directory entry names the next
 * level's table by the table's offset in video memory. The card caches the
 * translations it reads, so a change to any table BAR1 translates through
 * takes effect only once BAR1's cached translations are invalidated, as
 * NVIDIA's driver does after it replaces its own root page directory for BAR1
 * with the firmware's.
 *
 * BAR1's tables are this driver's own. They map pages only in BAR1's window,
 * the part of BAR1 a mapping may use. The window is BAR1 less its last 4 KiB,
 * which NVIDIA's driver never maps, and, by this driver's own limit, at most
 * the 512 MiB that the tables' one level 0 directory covers. The tables hold a
 * page table for each 2 MiB of the window or part of it, so a later mapping
 * only writes page table entries, where NVIDIA's driver allocates each page
 * table only once a mapping needs it. A boot console is a boot framebuffer
 * that may still be drawn in. When bring-up keeps one, the tables map its
 * bytes from the start of BAR1 onto the same offsets of video memory, so a
 * boot console lying at the start of both stays reachable at the start of BAR1
 * while the firmware runs. The tables take effect once they lie in video
 * memory, entry 0 of the firmware's root page directory names their level 2
 * directory and BAR1's cached translations are invalidated, in that order.
 * Since entry 0 covers far more than BAR1, the tables then translate every
 * offset of BAR1. An entry is sparse when it names nothing and an access
 * through it does not fault. Every page table entry mapping no console byte is
 * sparse, as is every directory entry the window does not use, so an access to
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
 * in system memory can deadlock PCIe transactions. `write_bar1_page_tables`
 * builds a copy of the tables for its caller to write into video memory, one
 * block of 4 KiB tables in the diagram's order: the directories of levels 2, 1
 * and 0, then the page tables in the order of the 2 MiB they cover. Each page
 * table maps 4 KiB pages, the size NVIDIA's driver maps the console with. A
 * page table of larger pages maps 64 KiB or 128 KiB pages, whichever size BAR1
 * is set up with, and the layout of a page table of 4 KiB pages does not
 * depend on that size, which this driver does not read.
 */

/**
 * @brief How many bytes from the start of BAR1 lie in BAR1's window, the part
 * of BAR1 a mapping may use. The window is BAR1 less its last 4 KiB, which
 * NVIDIA's driver never maps, rounded down to whole 4 KiB pages and, by this
 * driver's own limit, at most 512 MiB. The call never blocks.
 * @param bar1_bytes BAR1's size.
 * @return The window's bytes, 0 when BAR1 holds less than 8 KiB and so no
 * whole 4 KiB page before its last 4 KiB.
 */
uint64_t bar1_window_bytes(uint64_t bar1_bytes);

/**
 * @brief How many bytes BAR1's tables take for a window of `window_bytes`
 * bytes: 4 KiB for each of the three directories and 4 KiB for each page
 * table, one page table for each 2 MiB of the window or part of it. The call
 * never blocks.
 * @param window_bytes The window's bytes, as `bar1_window_bytes` gives them.
 * @return The bytes, a multiple of 4 KiB. It is 0 when `window_bytes` is 0,
 * leaving nothing to map, when `window_bytes` is not a multiple of 4 KiB, or
 * when `window_bytes` exceeds the 512 MiB one level 0 directory covers.
 */
uint64_t bar1_page_table_bytes(uint64_t window_bytes);

/**
 * @brief Write BAR1's tables into `tables` for a window of the first
 * `window_bytes` bytes of BAR1, mapping the first `console_bytes` of them onto
 * the same offsets of video memory in 4 KiB pages. Every directory entry the
 * window uses names a table, and every page table entry mapping a console byte
 * names a page in video memory. Each holds the offset of what it names in its
 * address field, in 4 KiB units, and video memory in its aperture field, which
 * names the memory that offset lies in. A page table entry naming a page also
 * has its valid bit set. Every other field is 0, the volatile bit included. In
 * an entry naming memory, that bit asks the card not to cache the memory, and
 * NVIDIA's driver leaves it 0 in its entries for video memory. In an entry
 * naming nothing, it marks the entry sparse. A page table entry's kind field
 * gives the layout of its page's memory, and kind 0 is NVIDIA's kind for
 * memory laid out row by row as a framebuffer is. Every other entry holds
 * NVIDIA's sparse value instead, which names nothing and has its volatile bit
 * set. The call never blocks.
 * @param tables The CPU's copy of the tables, which must hold
 * `bar1_page_table_bytes(window_bytes)` bytes. The call writes all of them
 * unless it returns 0.
 * @param tables_offset Where in video memory the caller must write the copy.
 * Since the console takes the first `console_bytes` bytes of video memory, the
 * tables must start at `console_bytes` or past it.
 * @param window_bytes The window's bytes, as `bar1_window_bytes` gives them.
 * @param console_bytes The console's size in bytes, 0 when bring-up keeps no
 * console.
 * @return The value entry 0 of the firmware's root page directory must hold
 * for BAR1's window to map through the tables, the directory entry naming the
 * level 2 directory at `tables_offset`. It is 0 when `bar1_page_table_bytes`
 * refuses `window_bytes`, when `console_bytes` is not a multiple of 4 KiB (the
 * page holding the console's last bytes would map bytes past the console),
 * when `console_bytes` exceeds `window_bytes` (the tables map pages only in
 * the window), when `tables_offset` is not a multiple of 4 KiB (an entry's
 * address field counts 4 KiB units), when the tables would start inside the
 * console, or when the tables would end past 128 GiB (the field holds only
 * offsets below 128 GiB). `tables` is then left as it is. No directory entry
 * naming video memory is 0.
 */
uint64_t write_bar1_page_tables(
    uint64_t* tables,
    uint64_t tables_offset,
    uint64_t window_bytes,
    uint64_t console_bytes
);

/**
 * @brief Check that the video memory region 0 the GSP's firmware reports can
 * be the console's memory, which BAR1's tables map. When NVIDIA's driver keeps
 * video memory for a boot console, it takes the firmware's region 0 whole as
 * the console's memory, so the tables map region 0. Region 0 must fit BAR1's
 * window, as `bar1_window_bytes` gives it for `bar1_bytes`: NVIDIA's driver
 * maps nothing in the last 4 KiB of BAR1 and so cannot map a region 0 that
 * fills BAR1, and the window's 512 MiB limit is this driver's own. The rules
 * that follow are this driver's own too, where NVIDIA's driver maps region 0
 * from wherever it starts, reserved or not. Region 0 must start at 0, since
 * the tables map BAR1's offsets onto the same offsets of video memory, must
 * hold `reserved_console_bytes`, so the boot framebuffer lies wholly inside
 * the mapping, and must hold whole 4 KiB pages. It must also be reserved, so
 * no video memory allocation can overwrite the console. The call never blocks.
 * @param configuration The static configuration `read_static_configuration`
 * read.
 * @param reserved_console_bytes The bytes the firmware was asked to leave
 * alone for the boot console, as `boot_console_bytes` gives them.
 * @param bar1_bytes BAR1's size.
 * @param console_bytes_out Receives region 0's size, the console's, on `OK`: a
 * multiple of 4 KiB, at least `reserved_console_bytes` and at most the
 * window's bytes. It is left as it is otherwise.
 * @return `OK`, or `ERR_BAD_STATE`, logged, when region 0 is not reserved,
 * does not start at 0, holds fewer than `reserved_console_bytes` bytes, ends
 * inside a 4 KiB page or does not fit the window.
 */
int32_t check_console_region(
    const static_configuration& configuration,
    uint64_t reserved_console_bytes,
    uint64_t bar1_bytes,
    uint64_t& console_bytes_out
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_PAGE_TABLES_H
