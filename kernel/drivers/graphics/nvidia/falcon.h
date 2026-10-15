// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// How long a reset waits for a falcon to report it is ready for one before going on without the report, 150 us as
// in NVIDIA's driver
constexpr uint64_t FALCON_READY_TIMEOUT_NS = 150000;

// How long each wait for a falcon's memories or its core selection may take, NVIDIA's default timeout of 4 s for a
// GPU in graphics mode rather than compute mode
constexpr uint64_t FALCON_TIMEOUT_NS = 4000000000;

/**
 * @brief Reset the GSP's falcon, the microcontroller in the GSP that runs
 * FWSEC, into the state NVIDIA's driver loads FWSEC into. The falcon has a
 * falcon core and, when its hardware configuration says so, a RISC-V core,
 * and it runs whichever core its RISC-V boot control register selects.
 * FWSEC runs on the falcon core.
 * NVIDIA's driver resets a falcon by disabling and then enabling it, and the
 * GSP's falcon has no bit in the chip's engine enable register, so each of
 * the two is a full reset. Each full reset gives the falcon up to
 * `ready_timeout_ns` to report it is ready for a reset, going on without the
 * report when it does not come, sets and then releases the GSP's engine
 * reset, waits for the falcon to finish clearing its instruction and data
 * memories, IMEM and DMEM, and selects the falcon core if the RISC-V core is
 * selected. After both full resets, as NVIDIA's reset ends, it selects the
 * falcon core again if the RISC-V core is selected, waits for the memories
 * once more, and writes `boot0` to the falcon's RM register, which NVIDIA's
 * driver sets to the chip's `PMC_BOOT_0` value as it finishes enabling a
 * falcon.
 * The waits spin on the calling CPU instead of sleeping, so a slow falcon can
 * hold the CPU for up to twice `ready_timeout_ns` and six times `timeout_ns`.
 * @param regs A view of BAR0. The card's boot firmware must have finished, as
 * `wait_for_boot_firmware` waits for, since the GSP runs it and the reset
 * would stop it.
 * @param boot0 The chip's `PMC_BOOT_0` value.
 * @param ready_timeout_ns How long each wait for the ready report may take,
 * `FALCON_READY_TIMEOUT_NS` at bring-up.
 * @param timeout_ns How long each wait for the memories or the core selection
 * may take, `FALCON_TIMEOUT_NS` at bring-up. Each wait reads its register at
 * least once, even for a timeout of 0. NVIDIA's driver counts its default
 * timeout once for a whole call rather than for each wait.
 * @return `OK`, or `ERR_TIMEOUT` when the falcon has not finished clearing
 * its memories, or the GSP has not confirmed its switch to the falcon core,
 * within `timeout_ns` of that wait's start. NVIDIA's driver goes on after
 * either wait runs out, so both failures are this driver's, to stop bring-up
 * before FWSEC is loaded into a falcon that has not finished its reset. The
 * cause is logged, and the RM register is left unwritten.
 */
int32_t reset_gsp_falcon(registers& regs, uint32_t boot0, uint64_t ready_timeout_ns, uint64_t timeout_ns);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H
