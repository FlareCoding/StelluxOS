// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OUTPUTS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OUTPUTS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_ids.h"
#include "drivers/graphics/nvidia/displayport.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * An output, a static digital display id whose signal is DisplayPort, with the
 * facts display common gives about it, its lanes and link rate kept in
 * NVIDIA's codes, which `rm.h` gives beside the fields they come from.
 */
struct display_output {
    uint32_t display_id;          // a single bit
    bool     is_connected;        // among the connected display ids `read_display_ids` gave
    uint8_t  lanes_code;          // the lanes the board supports, as `rm::DIGITAL_LANES_SHIFT`'s field codes them
    uint8_t  link_rate_code;      // the highest link rate, as `rm::DIGITAL_LINK_RATE_SHIFT`'s field codes it
    bool     is_limited_to_60_hz; // the firmware reports the display id as qualified only at 60 Hz refresh rates
    bool     prefers_rbr;         // the link must be trained at RBR (the reduced bit rate, 1.62 Gbps) whenever possible
    uint8_t  phy_repeaters;       // the PHY repeaters the firmware found on the link
};

/**
 * The outputs, numbered from 0 in ascending order of their display id's bit.
 */
struct display_outputs {
    uint8_t        count;
    display_output outputs[DISPLAY_IDS_MAX]; // the first `count` entries
};

/**
 * @brief For each static digital display id in ascending order of its bit, ask
 * display common for its digital facts, each request carrying its parameters
 * zeroed but for the display id, and give the outputs, those whose signal is
 * DisplayPort. A static digital display id whose signal is not DisplayPort, or
 * whose digital facts the firmware refuses with `ERR_FIRMWARE`, is logged and
 * is not an output, and the read goes on with the others. NVIDIA's modesetting
 * module instead asks for the digital facts only of a display id whose output
 * resource, the display engine's unit driving it, is serial (NVIDIA's SOR),
 * tells whether the display id drives DisplayPort from the protocol display
 * common reports for that unit and keeps the connector of a display id whose
 * request is refused, taking its flags as 0. Its DisplayPort code also asks
 * for them and goes on when they are refused.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param ids The display ids `read_display_ids` gave.
 * @param out Receives on `OK` the outputs, and is left unchanged on an error.
 * @return `OK`, or what `firmware::control` returns, logged as its doc
 * describes, for any request but one the firmware refuses with `ERR_FIRMWARE`.
 */
int32_t read_display_outputs(firmware& fw, rm::handle display_common, const display_ids& ids, display_outputs& out);

/**
 * @brief Give the rate a link rate code stands for, as display common codes a
 * highest rate both in a display id's digital facts and in an output
 * resource's DisplayPort facts.
 * @param code The code, read as its field's 3 bits.
 * @return The rate, RBR for a code NVIDIA defines no rate for, as NVIDIA's
 * DisplayPort library takes it.
 */
displayport::link_rate link_rate_of_code(uint32_t code);

/**
 * @brief Give the most lanes and the highest rate a link on an output's
 * connector takes, from the codes display common gave, the rate capped by the
 * GPU's own highest.
 * @param output The output, whose codes are read as their fields' 3 bits.
 * @param gpu_max_rate The highest rate the GPU's links run at.
 * @return 1, 2 or 4 lanes for a board supporting one, two or four, and 4 for
 * one supporting eight, as no link trains more. A lane code for none, or one
 * NVIDIA defines no count for, gives 0 lanes, which leaves the output no link
 * configuration. A link rate code NVIDIA defines no rate for counts as RBR, as
 * NVIDIA's DisplayPort library takes it. NVIDIA's library instead reads no
 * connector's lanes, taking a link's from its monitor's DPCD alone, and caps
 * the rate by the connector's only when the firmware or a registry key asks
 * for it.
 */
displayport::link_limits connector_link_limits(const display_output& output, displayport::link_rate gpu_max_rate);

/**
 * @brief Give the facts deciding which link configurations an output can run
 * and how a stream fills each: its connector's limits from
 * `connector_link_limits` with the GPU's highest rate, its monitor's from
 * `monitor_link_limits`, whether the monitor takes enhanced framing, and
 * whether the GPU's links support FEC and take the increased watermark limits.
 * @param output The output.
 * @param dpcd The capabilities `read_output_dpcd` read of its monitor.
 * @param gpu The GPU's own DisplayPort facts.
 * @return The output's link facts.
 */
displayport::link_facts output_link_facts(
    const display_output& output,
    const dpcd_capabilities& dpcd,
    const chip_displayport& gpu
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OUTPUTS_H
