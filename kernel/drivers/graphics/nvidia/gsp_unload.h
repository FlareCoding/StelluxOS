// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_UNLOAD_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_UNLOAD_H

#include "common/types.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// NVIDIA's number for the request that asks the GSP's firmware to unload, which the firmware's reply carries too
constexpr uint32_t FUNCTION_UNLOADING_GUEST_DRIVER = 0x2F;

/**
 * @brief Ask the GSP's firmware to unload, then wait for the GSP's processor
 * (its RISC-V core, which runs the firmware) to be suspended, as NVIDIA's
 * driver does before it resets the GSP. The request goes out with NVIDIA's
 * arguments for an unload outside a power transition (all 0), through
 * `gsp::request`. The wait for the suspend follows as `wait_for_gsp_suspend`
 * describes, whether or not the request succeeded, as in NVIDIA's driver,
 * which drops the wait's result where this call logs it and returns it when
 * the request succeeded. While NVIDIA's thread-state timeout is on, its wait
 * for the suspend ends only once a default timeout begun before the request is
 * spent, time the wait for the reply takes up as well, while this wait always
 * has `suspend_timeout_ns` of its own. NVIDIA's driver also writes the GSP's
 * log out before the GSP's reset, a log this driver never reads. The call
 * spins on the calling CPU without sleeping, through the wait for the reply,
 * which `gsp::request` bounds, and then for up to `suspend_timeout_ns`.
 * @param conversation The driver's `gsp`, whose status queue must be linked.
 * @param regs A view of BAR0, for the GSP falcon's first mailbox, which the
 * wait for the suspend reads.
 * @param reply_timeout_ns How long the wait for the reply may take, as
 * `gsp::request` takes it, `RECEIVE_TIMEOUT_NS` at unload.
 * @param suspend_timeout_ns How long the wait for the suspend may take,
 * `DEFAULT_TIMEOUT_NS` at unload.
 * @return `OK` once the request succeeds and the processor reads as suspended,
 * or else the request's error if it failed and the suspend's if not: what
 * `gsp::request` returns, logged as its doc describes, or the `ERR_TIMEOUT`
 * `wait_for_gsp_suspend` logs and returns.
 */
int32_t unload_gsp_firmware(
    gsp& conversation,
    const registers& regs,
    uint64_t reply_timeout_ns,
    uint64_t suspend_timeout_ns
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_UNLOAD_H
