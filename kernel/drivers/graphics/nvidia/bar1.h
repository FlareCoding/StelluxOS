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

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_BAR1_H
