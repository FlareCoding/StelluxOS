// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_UNLOAD_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_UNLOAD_H

#include "common/types.h"
#include "drivers/graphics/nvidia/cpu_sequencer.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// NVIDIA's number for the request that asks the GSP's firmware to unload, which the firmware's reply carries too
constexpr uint32_t FUNCTION_UNLOADING_GUEST_DRIVER = 0x2F;

/**
 * @brief Ask the GSP's firmware to unload, then wait for the GSP's processor
 * (its RISC-V core, which runs the firmware) to be suspended, as NVIDIA's
 * driver does before it resets the GSP. The request goes out with NVIDIA's
 * arguments for an unload outside a power transition (all 0), the firmware is
 * told that it waits, and the reply is awaited with `receive_awaited`, so a
 * CPU sequencer sent before the reply runs as soon as its message is read,
 * with no log line first. The wait for the suspend follows as
 * `wait_for_gsp_suspend` describes, whether or not the request succeeded, as
 * in NVIDIA's driver, which drops the wait's result where this call logs it
 * and returns it when the request succeeded. While NVIDIA's thread-state
 * timeout is on, its wait for the suspend ends only once a default timeout
 * begun before the request is spent, time the wait for the reply takes up as
 * well, while this wait always has `suspend_timeout_ns` of its own. NVIDIA's
 * driver also skips the request when its GSP has crashed or the GPU is in
 * reset, lost, shut down, below full power or without access to system memory,
 * waits up to 1 s for room in the command queue, whereas this request fails at
 * once on a full queue, and writes the GSP's log out before the GSP's reset, a
 * log this driver never reads. The call spins on the calling CPU without
 * sleeping, through the wait for the reply, which `receive_awaited` bounds,
 * and then for up to `suspend_timeout_ns`.
 * @param queues The message queues, whose status queue must be linked.
 * @param regs A view of BAR0, for the doorbell, the sequencer's steps and the
 * GSP falcon's first mailbox, as `receive_awaited` requires.
 * @param sequencer What a sequencer's steps take, as for `receive_awaited`.
 * @param payload Where each message's payload is copied, as for
 * `receive_awaited`.
 * @param payload_capacity How many bytes `payload` holds.
 * @param reply_deadline_ns The deadline of the wait for the reply, as
 * `receive_awaited` takes it, a time as `clock::now_ns` reads it,
 * `RECEIVE_TIMEOUT_NS` after the request at unload.
 * @param suspend_timeout_ns How long the wait for the suspend may take,
 * `DEFAULT_TIMEOUT_NS` at unload.
 * @return `OK` once the reply's result is 0 and the processor reads as
 * suspended, or else the request's error if it failed and the suspend's if
 * not: what `message_queues::send` returns when the request cannot be queued,
 * which is logged, what `receive_awaited` returns when its wait ends without
 * the reply, logged as its doc describes, `ERR_FIRMWARE`, logged with the
 * status, when the reply's result is not 0, or the `ERR_TIMEOUT`
 * `wait_for_gsp_suspend` logs and returns.
 */
int32_t unload_gsp_firmware(
    message_queues& queues,
    registers& regs,
    const cpu_sequencer_context& sequencer,
    uint8_t* payload,
    uint32_t payload_capacity,
    uint64_t reply_deadline_ns,
    uint64_t suspend_timeout_ns
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_UNLOAD_H
