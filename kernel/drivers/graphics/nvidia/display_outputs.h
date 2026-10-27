// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OUTPUTS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OUTPUTS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/display_ids.h"
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
    bool     is_limited_to_60_hz; // the display id may only show modes of a 60 Hz refresh rate
    bool     prefers_rbr;         // the link must be trained at RBR (the reduced bit rate, 1.62 Gbps) whenever possible
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

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_OUTPUTS_H
