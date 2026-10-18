// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CPU_SEQUENCER_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CPU_SEQUENCER_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// NVIDIA's number for the message that carries the CPU sequencer from the GSP's firmware
constexpr uint32_t FUNCTION_RUN_CPU_SEQUENCER = 0x1002;

// What the CPU sequencer's steps take from bring-up besides BAR0's registers
struct cpu_sequencer_context {
    uint64_t register_bytes;          // how many bytes of BAR0 are mapped, past which no register step reaches
    uint32_t boot0;                   // the chip's `PMC_BOOT_0` value, for the GSP falcon's reset
    uint64_t libos_arguments_address; // as `resume_gsp_firmware` takes it
    uint32_t app_version;             // as `resume_gsp_firmware` takes it
    uint64_t ready_timeout_ns;        // `FALCON_READY_TIMEOUT_NS` at bring-up
    uint64_t deadline_ns;             // a `clock::now_ns` time all waits but delays and ready waits count against
};

/**
 * @brief Run the CPU sequencer, the list of register, delay and falcon steps
 * the GSP's firmware sends in a `FUNCTION_RUN_CPU_SEQUENCER` message for the
 * driver to run, as NVIDIA's driver runs it. The payload starts with how many
 * words the firmware's buffer holds, how many words of it the steps take and
 * a save area of 8 words. The steps follow, each an opcode and the words of
 * arguments that opcode takes. They write, modify, poll and store registers,
 * delay, reset the GSP's falcon as `reset_falcon` does and then let its DMA
 * engine run with no address space bound, start the falcon, wait for its
 * halt, and have SEC2 resume the GSP's firmware as `resume_gsp_firmware`
 * does. The steps run in order, and the first that fails or is refused ends
 * the run, as in NVIDIA's driver.
 * Two refusals and one ending are this driver's own. A payload too short for
 * its 40-byte header or for the words its steps take is refused, since the
 * bytes past it are not the message's, where NVIDIA's driver reads its copy
 * with no check of its length. A register step naming an offset that is not a
 * multiple of 4 or that lies past `register_bytes` is refused, since an
 * access past the mapping faults the CPU and an unaligned one faults on
 * aarch64, where NVIDIA's driver accesses whatever offset a step names. A
 * reset that times out ends the run, where NVIDIA's reset step cannot fail.
 * The reset times out only once `deadline_ns` has passed, after which every
 * later wait NVIDIA's run gives its default timeout, the resume's wait for
 * SEC2 among them, fails unless met at once.
 * The waits spin on the calling CPU without sleeping. A delay lasts its
 * microseconds, and each ready wait of a falcon step lasts at most
 * `ready_timeout_ns`, as in NVIDIA's driver. Every other wait counts against
 * `deadline_ns`, as NVIDIA's driver counts it against its thread-state budget
 * (one default timeout that the waits of a call share, started again just
 * before its wait for the firmware's messages), so `attach()` sets
 * `deadline_ns` to the time `DEFAULT_TIMEOUT_NS` after its own wait for the
 * messages starts. A poll ends at `deadline_ns`, or sooner at its own timeout
 * in microseconds, without the OS tick NVIDIA's driver adds, and a poll whose
 * timeout is 0, NVIDIA's default, ends only at `deadline_ns`. Each falcon
 * step that waits is given the time left until `deadline_ns` as its timeout,
 * so a step's later waits may end past it by as long as its earlier waits
 * took.
 * @param regs A view of BAR0. Booter load must have run on SEC2's falcon, as
 * `resume_gsp_firmware` requires, with nothing resetting SEC2 since.
 * @param context What the steps take from bring-up.
 * @param payload The message's payload. A register store step writes the
 * value it reads into the payload's save area, as NVIDIA's driver writes its
 * copy of the message, which nothing reads afterwards.
 * @param payload_bytes How many bytes `payload` holds.
 * @return `OK` once every step ran, or `ERR_BAD_MESSAGE` when the payload is
 * shorter than its 40-byte header, when the words its steps take reach the
 * words its buffer holds or pass the words the payload holds after its
 * header, or when a step is refused for arguments running past the words its
 * steps take, an unknown opcode, a store index past the save area or a
 * register offset as above, the steps before it having run. Otherwise the
 * error of the first step that fails: `ERR_TIMEOUT` from a poll, the reset,
 * the halt wait or the resume, or `ERR_BAD_STATE` from the resume. Every
 * refusal and failure is logged.
 */
int32_t run_cpu_sequencer(
    registers& regs,
    const cpu_sequencer_context& context,
    uint8_t* payload,
    uint32_t payload_bytes
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CPU_SEQUENCER_H
