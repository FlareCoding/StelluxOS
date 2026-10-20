// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// The GPU interrupt vectors (NVIDIA's numbers for the GPU's interrupts) of a leaf, a register of the CPU interrupt tree
// (the GPU's registers reporting its interrupts to the CPU), so vector `v` is bit `v % 32` of leaf `v / 32`
constexpr uint32_t VECTORS_PER_LEAF = 32;

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
 * leaf register for it. Here later rows of the GSP's engine are not read, and
 * a table without a row for the GSP fails the call, as does a GSP vector past
 * those leaves, 0xFFFFFFFF included. The call spins on the calling CPU without
 * sleeping, through the wait for the reply, which `gsp::request` bounds.
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
 * `rm::KERNEL_INTERRUPT_ROWS_MAX` rows or holds no row for the GSP within its
 * count, or gives the GSP a vector at or above `leaf_count` times
 * `VECTORS_PER_LEAF`. Every error but the control's is logged here.
 */
int32_t read_gsp_interrupt_vector(
    gsp& conversation,
    rm::handle client,
    rm::handle subdevice,
    uint32_t leaf_count,
    uint64_t timeout_ns,
    uint32_t& vector
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_INTERRUPTS_H
