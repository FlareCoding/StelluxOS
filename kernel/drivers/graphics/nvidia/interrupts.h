// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// The GPU interrupt vectors (NVIDIA's numbers for the GPU's interrupts) of a leaf, a register of the CPU interrupt tree
// (the GPU's registers reporting its interrupts to the CPU), so vector `v` is bit `v % 32` of leaf `v / 32`
constexpr uint32_t VECTORS_PER_LEAF = 32;

// The leaves of a subtree, the leaves reporting to one bit of the tree's top level, so leaf `l` reports to bit `l / 2`
constexpr uint32_t LEAVES_PER_SUBTREE = 2;

// The interrupt source (a bit of the interrupt status) of the GSP's falcon (the microcontroller in the GSP) that the
// firmware raises when it queues a message it sends unasked (NVIDIA's SWGEN0)
constexpr uint32_t GSP_MESSAGE_SOURCE = 1u << 6;

// What `acknowledge_gsp_interrupt` returns for a card that has stopped responding, whose registers then read all ones
constexpr uint32_t UNRESPONSIVE_CARD_SOURCES = 0xFFFFFFFF;

// The GPU interrupt vectors the driver services, each below the tree's leaves times `VECTORS_PER_LEAF`
struct interrupt_vectors {
    uint32_t gsp;         // the GSP's, on which its firmware signals the driver
    uint32_t display;     // the display's, on which the heads' vertical blanks arrive, read only with `has_display`
    bool     has_display; // whether the driver services the display's vector
};

/**
 * @brief Read the GSP's and the display's vectors from the kernel interrupt
 * table the firmware returns, each the stall vector of its engine's first row,
 * usable below `leaf_count` times `VECTORS_PER_LEAF` and without a PMC mask.
 * Spins until the reply or `timeout_ns`, and fills `vectors` only on `OK`.
 * @return `OK`, with `has_display` false when `is_display_enabled` (whether
 * the fuses leave the display enabled) is false or no usable display row
 * exists, `ERR_NO_MEMORY`, what `gsp::control` returns, or `ERR_BAD_MESSAGE`
 * for a table past `rm::KERNEL_INTERRUPT_ROWS_MAX` rows or without a usable
 * GSP row. Every error but the control's is logged here.
 */
int32_t read_interrupt_vectors(
    gsp& conversation,
    rm::handle client,
    rm::handle subdevice,
    uint32_t leaf_count,
    bool is_display_enabled,
    uint64_t timeout_ns,
    interrupt_vectors& vectors
);

/**
 * @brief Let only the driver's vectors raise the card's interrupt: clear every
 * leaf's enable, then enable the GSP's vector and, with `has_display`, the
 * display's, each in its leaf and its subtree's bit of the top level. A vector
 * already pending raises the interrupt at once. Owns every enable of the tree
 * with `disable_cpu_interrupt_tree`, touches only BAR0 and takes no lock.
 */
void enable_cpu_interrupt_tree(registers& regs, uint32_t leaf_count, const interrupt_vectors& vectors);

/**
 * @brief Stop every vector of the CPU interrupt tree from raising the card's
 * interrupt: clear every subtree's bit in the top level's enable, then every
 * bit of each leaf's enable, as NVIDIA's driver does as it unloads. The tree's
 * pending bits are left as they are. The call touches only BAR0 and takes no
 * lock.
 * @param regs A view of BAR0.
 * @param leaf_count How many leaves the chip's CPU interrupt tree has, as
 * `enable_cpu_interrupt_tree` took it.
 */
void disable_cpu_interrupt_tree(registers& regs, uint32_t leaf_count);

/**
 * @brief Acknowledge the GSP's interrupt from the card's interrupt handler:
 * clear the vector's pending bit in its leaf, as NVIDIA's driver clears an
 * engine's vector before servicing the engine, then read the sources the GSP's
 * falcon has pending for the host (the CPU). A source is pending for the host
 * when it is set in the falcon's status and passed by both the falcon core's
 * mask and destination, or by both the RISC-V core's. When a source is
 * pending, write the sources read to the falcon's clear register and retrigger
 * the falcon's interrupt, which sends the interrupt again if any source is
 * still pending. NVIDIA's driver clears only HALT (the source the firmware
 * raises when its crash report needs the host) and SWGEN0 of the sources it
 * reads, while every source read is written to the clear here, since a source
 * left pending would send the interrupt again at the retrigger and raise the
 * next one at once. NVIDIA's driver reads the firmware's messages between the
 * clear and the retrigger, while the caller must read them after the call.
 * That loses none, since a message the firmware sends unasked after the clear
 * raises SWGEN0 again. The call touches only BAR0 and takes no lock.
 * @param regs A view of BAR0.
 * @param vector The GSP's vector, as `enable_cpu_interrupt_tree` took it.
 * @return The sources written to the clear, 0 when none was pending.
 * `GSP_MESSAGE_SOURCE` among them means the firmware signalled a message,
 * unless they are `UNRESPONSIVE_CARD_SOURCES`, every bit set, which means the
 * card has stopped responding, as its registers then read all ones.
 */
uint32_t acknowledge_gsp_interrupt(registers& regs, uint32_t vector);

/**
 * @brief Clear the display's vector in its leaf when it is pending, from the
 * card's interrupt handler, before the heads' interrupts are serviced. The
 * call touches only BAR0 and takes no lock.
 * @return true when the vector was pending.
 */
bool acknowledge_display_interrupt(registers& regs, uint32_t vector);

/**
 * @brief Re-arm the card's MSI at the end of the card's interrupt handler, as
 * NVIDIA's driver does on every chip of the chip table while MSI is on, at the
 * end of each run of its interrupt handler, whether or not it found an
 * interrupt pending: write 0 to the register at 0x704 of BAR0's mirror of the
 * card's PCI configuration space (NVIDIA's NV_XVE_CYA_2), a write whose value
 * the card ignores. NVIDIA's handler does so only while the GPU's state is
 * loaded and its interrupts are enabled in hardware, and skips the write when
 * its check of register access fails. The call touches only BAR0 and takes no
 * lock.
 * @param regs A view of BAR0.
 */
void rearm_msi(registers& regs);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H
