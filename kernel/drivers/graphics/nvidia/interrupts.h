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

/**
 * @brief Ask the GSP's firmware for the kernel interrupt table (for each GPU
 * unit whose interrupts NVIDIA's driver services, the vectors on which they
 * arrive) and find in it the GSP's own vector, on which the firmware signals
 * the driver. An engine is one of the GPU's units that NVIDIA's interrupt code
 * serves, and a vector here is a GPU interrupt vector, NVIDIA's number for one
 * of the GPU's interrupts, not an MSI vector of the PCI layer. NVIDIA's driver
 * sends this control (a command to an object the firmware keeps) as it starts
 * the GPU and builds its interrupt table from the reply. The control goes
 * through `gsp::control` to the internal subdevice (the object standing for
 * this GPU) of the internal client (the client the firmware keeps for the
 * controls NVIDIA's driver sends on its own behalf). Its 2068 bytes of
 * parameters are zeroed and allocated on the unprivileged heap for the call,
 * where NVIDIA's driver sends an allocation it has not zeroed. The table's
 * count of rows may not exceed `rm::KERNEL_INTERRUPT_ROWS_MAX`, as NVIDIA's
 * driver requires. Each row gives an engine's stall vector, on which the
 * interrupts NVIDIA's driver services with the engine's handler arrive, and
 * its nonstall vector, on which the engine's notifications arrive. The GSP's
 * vector is the stall vector of the first row of the GSP's engine, the vector
 * NVIDIA's driver clears as it services the GSP's interrupts, which it
 * services as stall interrupts. NVIDIA's driver keeps every row of the GSP's
 * engine and accepts a table without such a row. It copies each row's vectors
 * into its table unchecked and takes 0xFFFFFFFF for no vector, but never
 * enables a vector past the leaves of the CPU interrupt tree, which has no
 * leaf register for it, nor, but for the timer's row, the vectors of a row
 * with a PMC mask (the engine's bits in the GPU's PMC interrupt register,
 * which is not part of the CPU interrupt tree). Here later rows of the GSP's
 * engine are not read, and a table without a row for the GSP fails the call,
 * as does a GSP vector past those leaves, 0xFFFFFFFF included, or a PMC mask
 * in the GSP's row, which NVIDIA's driver accepts. The call spins on the
 * calling CPU without sleeping, through the wait for the reply, which
 * `gsp::request` bounds.
 * @param conversation The driver's `gsp`, whose status queue must be linked.
 * @param client The internal client's handle, as `static_configuration` holds
 * it.
 * @param subdevice The internal subdevice's handle, as `static_configuration`
 * holds it.
 * @param leaf_count How many leaves the chip's CPU interrupt tree has, as
 * `chip_info::interrupt_leaf_count` gives it.
 * @param timeout_ns How long the wait for the reply may take, as
 * `gsp::request` takes it, `RECEIVE_TIMEOUT_NS` at bring-up.
 * @param vector Receives the GSP's vector, below `leaf_count` times
 * `VECTORS_PER_LEAF`, on `OK`, and is left unchanged on an error.
 * @return `OK`, `ERR_NO_MEMORY` when the parameters cannot be allocated, what
 * `gsp::control` returns when the control fails, logged as its doc describes,
 * or `ERR_BAD_MESSAGE` when the table counts more than
 * `rm::KERNEL_INTERRUPT_ROWS_MAX` rows, holds no row for the GSP within its
 * count, gives the GSP a vector at or above `leaf_count` times
 * `VECTORS_PER_LEAF` or gives the GSP's row a PMC mask. Every error but the
 * control's is logged here.
 */
int32_t read_gsp_interrupt_vector(
    gsp& conversation,
    rm::handle client,
    rm::handle subdevice,
    uint32_t leaf_count,
    uint64_t timeout_ns,
    uint32_t& vector
);

/**
 * @brief Let the GSP's vector alone raise the card's interrupt: clear every
 * bit of each leaf's enable, then set the vector's bit in its leaf's enable
 * and its subtree's bit in the enable of the tree's top level. A pending
 * vector raises the card's interrupt only while both of those bits are set.
 * NVIDIA's driver clears each leaf's enable the same way before it enables any
 * vector, since an earlier driver can leave enables set, then enables at their
 * leaves the vectors of its table but those `read_gsp_interrupt_vector` says
 * it never enables, and enables its stall interrupts by setting at the top
 * level every subtree but those of its nonstall interrupts. As no other vector
 * is enabled at any leaf, setting the GSP's subtree alone raises the same
 * interrupts, unless the kernel interrupt table's subtree map, which this
 * driver does not read, gives that subtree to NVIDIA's nonstall interrupts.
 * This call and `disable_cpu_interrupt_tree` own every enable of the tree, so
 * any other vector the driver services must be enabled here with the GSP's.
 * The interrupt masks and destinations of the GSP's falcon, which decide which
 * of its interrupt sources reach the tree, are left as the firmware sets them,
 * as NVIDIA's driver only reads them. A vector already pending raises the
 * card's interrupt at once. The call touches only BAR0 and takes no lock.
 * @param regs A view of BAR0.
 * @param leaf_count How many leaves the chip's CPU interrupt tree has, as
 * `chip_info::interrupt_leaf_count` gives it.
 * @param gsp_vector The GSP's vector, which must be below `leaf_count` times
 * `VECTORS_PER_LEAF`, as `read_gsp_interrupt_vector` gives it.
 */
void enable_cpu_interrupt_tree(registers& regs, uint32_t leaf_count, uint32_t gsp_vector);

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
 * unless every bit is set, which means the card has stopped responding, as its
 * registers then read all ones.
 */
uint32_t acknowledge_gsp_interrupt(registers& regs, uint32_t vector);

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
