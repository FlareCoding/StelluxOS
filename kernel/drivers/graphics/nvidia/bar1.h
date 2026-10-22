// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

/**
 * @brief Read BAR1's block register, the register that binds BAR1 (the CPU's
 * window into the card's video memory) to video memory. It reads as the boot
 * firmware left it until the CPU sequencer's resume step, after which the
 * GSP's firmware rewrites it to send BAR1 through the firmware's own page
 * tables, and BAR1 no longer reaches the memory it reached before, such as a
 * boot framebuffer.
 * @param regs A view of BAR0.
 * @return The register's value.
 */
uint32_t read_bar1_block(const registers& regs);

/**
 * @brief Write `saved_block` back into BAR1's block register once the GSP
 * reads as stopped (its falcon halted and its RISC-V core not running), so
 * BAR1 reaches what it reached before the GSP's firmware ran. A register
 * reading `saved_block` is left alone. A register reading otherwise while the
 * GSP does not read as stopped is left unwritten, since a firmware still
 * running may rewrite it, and both values are logged. A write that does not
 * take is logged as an error. NVIDIA's driver never writes this register from
 * the CPU, so the write is this driver's own step.
 * @param regs A view of BAR0.
 * @param saved_block What `read_bar1_block` returned before the GSP's
 * firmware ran.
 */
void restore_bar1_block(registers& regs, uint32_t saved_block);

/**
 * @brief Invalidate BAR1's cached translations through the page tables whose
 * root page directory (the tables' top level) lies `root_offset` bytes into
 * video memory, as NVIDIA's driver does once it has replaced its own root page
 * directory for BAR1 with the firmware's. An invalidate is the card's
 * operation that drops cached translations from its TLBs (its caches of
 * translations). NVIDIA's driver limits a BAR's invalidates to one TLB, which
 * NVIDIA's code calls the hub's TLB, noting that a BAR's translations need
 * none of the graphics engine's TLBs invalidated. As NVIDIA's driver does, the
 * call waits for any invalidate the card is still running to finish, writes
 * the root page directory's offset with video memory as its aperture (the
 * memory the offset lies in), asks the card to drop every translation through
 * that directory from the hub's TLB alone, and waits for the card to finish.
 * The value the call writes to ask for the invalidate also sets its scope
 * field (the TLBs an invalidate covers), and every chip the chip table lists
 * has that field. Both waits share one deadline, `timeout_ns` after the call
 * starts, and spin on the calling CPU without sleeping.
 * @param regs A view of BAR0.
 * @param root_offset Bytes from the start of video memory to the root page
 * directory, which must lie below 2^60, the reach of the registers' 48 bits of
 * 4 KiB units, since past it the call would set bits outside the second
 * register's field. As those bits count 4 KiB units, the call names the 4 KiB
 * page holding the directory, as NVIDIA's driver does.
 * @param timeout_ns How long the two waits may take together.
 * @return `OK`, or `ERR_TIMEOUT`, logged, when the invalidate register's
 * trigger (the bit the card clears once an invalidate is done) still reads set
 * at the deadline: in the first wait, with no register written, or in the
 * second, with the invalidate asked for.
 */
int32_t invalidate_bar1_tlb(registers& regs, uint64_t root_offset, uint64_t timeout_ns);

/**
 * @brief Link the console's page tables into BAR1's: write `root_entry` as the
 * first entry of BAR1's root page directory through BAR0's window, read the
 * entry back through the window, and invalidate BAR1's cached translations
 * through that directory with `invalidate_bar1_tlb`. The console is a boot
 * framebuffer that may still be drawn in, its tables are those
 * `write_console_page_tables` builds, and BAR0's window is the 1 MiB of BAR0
 * that reaches video memory without page tables. NVIDIA's driver writes the
 * entries of its own root page directory into the firmware's and then
 * invalidates, in that order. The read-back between them completes the
 * window's writes in video memory before the invalidate, as NVIDIA's driver
 * requires for its own writes to page tables, flushing them with a read.
 * Refusing an entry that reads back as another value is this driver's own
 * step, since NVIDIA's driver reads back none of the entries it writes. The
 * call spins in the invalidate's waits without sleeping.
 * @param regs A view of BAR0.
 * @param root_offset Bytes from the start of video memory to BAR1's root page
 * directory. It must be a multiple of 8 below `BAR0_WINDOW_REACH`, as
 * `write_video_memory` requires.
 * @param root_entry The entry naming the console's tables, as
 * `write_console_page_tables` returns it.
 * @param timeout_ns How long the invalidate's two waits may take together.
 * @return `OK`, `ERR_BAD_STATE`, logged, with the write made and no invalidate
 * asked for, when the entry reads back as another value, or `ERR_TIMEOUT`,
 * logged, with the entry written, as `invalidate_bar1_tlb` describes.
 * `write_video_memory` refuses no single entry, so it never fails the call.
 */
int32_t link_console_page_tables(registers& regs, uint64_t root_offset, uint64_t root_entry, uint64_t timeout_ns);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_H
