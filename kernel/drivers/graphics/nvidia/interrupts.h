// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/firmware.h"
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

// The GPU interrupt vectors the driver services, each below the tree's leaves times `VECTORS_PER_LEAF`
struct interrupt_vectors {
    uint32_t gsp;         // the GSP's, on which its firmware signals the driver
    uint32_t display;     // the display's, on which the heads' vertical blanks arrive, read only with `has_display`
    bool     has_display; // whether the driver services the display's vector
};

// What `take_gsp_interrupt` found of the GSP's vector
enum class gsp_vector_reading : uint8_t {
    idle,     // not pending
    taken,    // pending, then disabled and cleared for the driver task
    all_ones, // its leaf read all ones, as on a card that stopped responding, and was left unwritten
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
 * @brief Read into `vector` the nonstall vector of the copy engine of engine
 * type `engine_type`, from the engine's first row in the kernel interrupt table
 * asked for through `fw`'s internal client.
 * @return `OK`, `ERR_INVALID_ARG` for a type of no copy engine,
 * `ERR_NO_MEMORY`, the control's error, or `ERR_BAD_MESSAGE` for a table of too
 * many rows, no row for the engine, or a row with a PMC mask or a vector
 * outside the `leaf_count` leaves. Every error but the control's is logged
 * here, and `vector` changes only on `OK`.
 */
int32_t read_copy_vector(firmware& fw, uint32_t engine_type, uint32_t leaf_count, uint32_t& vector);

/**
 * @brief Let only the driver's vectors raise the card's interrupt: clear every
 * leaf's enable, then enable the GSP's vector and, with `has_display`, the
 * display's, each in its leaf and its subtree's bit of the top level. A vector
 * already pending raises the interrupt at once. Only this call,
 * `enable_vector`, `mask_vector`, `take_gsp_interrupt`,
 * `service_gsp_interrupt` and `disable_cpu_interrupt_tree` write the tree's
 * enables. The call touches only BAR0 and takes no lock.
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
 * @brief Let `vector` raise the card's interrupt too, setting its bit in its
 * leaf's enable and its subtree's bit in the top level's. A vector already
 * pending raises the interrupt at once. The call touches only BAR0 and takes
 * no lock.
 * @param regs A view of BAR0.
 * @param vector A vector inside the CPU interrupt tree, as `read_copy_vector`
 * gives it.
 */
void enable_vector(registers& regs, uint32_t vector);

/**
 * @brief Stop `vector` from raising the card's interrupt, clearing its bit in
 * its leaf's enable and leaving the top level's, which the other vectors of
 * its subtree may need. The call touches only BAR0 and takes no lock.
 * @param regs A view of BAR0.
 * @param vector A vector inside the CPU interrupt tree, as `read_copy_vector`
 * gives it.
 */
void mask_vector(registers& regs, uint32_t vector);

/**
 * @brief Take the GSP's interrupt for the driver task, from the card's
 * interrupt handler: when the GSP's vector is pending in a leaf not reading
 * all ones, disable it and then clear it, writing only its bit, and leave the
 * GSP's falcon to the task, since a CPU sequencer the task runs can hold the
 * falcon in reset. The vector stays disabled until `service_gsp_interrupt`
 * enables it again. The call touches only BAR0 and takes no lock.
 * @return Whether the vector was idle or taken, or its leaf read all ones.
 */
gsp_vector_reading take_gsp_interrupt(registers& regs, uint32_t vector);

/**
 * @brief Service the GSP's interrupt from the driver task, once
 * `take_gsp_interrupt` has taken it or found its leaf reading all ones. When
 * the GSP's falcon has sources pending for the host (the CPU), clear them,
 * read the firmware's messages with `gsp::drain`, check the GSP's health, and
 * retrigger the falcon's interrupt unless `gsp::is_in_lockdown`. Then enable
 * the GSP's vector, which raises the interrupt at once if it is pending again.
 * Sources reading all ones, as on a card that stopped responding, are logged
 * and nothing is written. The call spins without sleeping as `gsp::drain`
 * does.
 */
void service_gsp_interrupt(registers& regs, gsp& conversation, uint32_t vector);

/**
 * @brief Clear the display's vector in its leaf when it is pending, from the
 * card's interrupt handler, before the heads' interrupts are serviced. The
 * call touches only BAR0 and takes no lock.
 * @return true when the vector was pending.
 */
bool acknowledge_display_interrupt(registers& regs, uint32_t vector);

/**
 * @brief Clear the nonstall `vector`, as `read_copy_vector` gives it, in its
 * leaf when it is pending, from the card's interrupt handler, then read the
 * leaf back so the clear lands before the handler reads the semaphores, since a
 * release after the clear raises the vector again. The call touches only BAR0
 * and takes no lock.
 * @return true when the vector was pending.
 */
bool acknowledge_nonstall_interrupt(registers& regs, uint32_t vector);

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
