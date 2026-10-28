#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_outputs.h"

namespace drivers::nvidia {

/**
 * A connected output's modes, the timings of its EDID that may be its mode in
 * the order `find_mode_candidates` ranks them.
 */
struct output_modes {
    uint8_t      count;
    uint8_t      indices[edid::TIMINGS_MAX]; // each mode's index in the EDID, the first `count` entries
    edid::timing timings[edid::TIMINGS_MAX]; // each mode's timing, the first `count` entries
};

/**
 * The head and window to show the output to light first, and the output tried
 * first.
 */
struct light_target {
    uint8_t head;
    uint8_t window;
    uint8_t first_output;
};

/**
 * The output to light first, the mode to light it at and the head and window
 * to show it.
 */
struct first_light {
    uint8_t      output;
    uint8_t      timing_index; // the mode's index in the output's EDID
    edid::timing timing;
    uint8_t      head;
    uint8_t      window;
};

/**
 * @brief Give the head and window to show the output to light first, the boot
 * head, else head 0, with that head's lowest window, and the output tried
 * first, the boot output, else output 0.
 * @param heads The heads `read_display_heads` gave.
 * @param active The heads `read_active_heads` gave for `heads.count` heads.
 * @param outputs The outputs `read_display_outputs` gave.
 * @return The head, below `heads.count`, the window, and the output tried
 * first, below `outputs.count` unless that is 0.
 */
light_target find_light_target(const display_heads& heads, const active_heads& active, const display_outputs& outputs);

/**
 * @brief Choose the output to light first and its mode, shown by the target's
 * head and window: the first mode of the first output that has one, the
 * outputs tried from the target's first output, then the others in ascending
 * order.
 * @param modes The modes of each of the `output_count` outputs.
 * @param output_count How many outputs `modes` holds.
 * @param target The head, window and first output `find_light_target` gave,
 * whose first output must be below `output_count` unless that is 0.
 * @param out Receives the choice when one is made, and is left unchanged
 * otherwise.
 * @return Whether a mode was chosen.
 */
bool choose_first_light(
    const output_modes* modes,
    uint8_t output_count,
    const light_target& target,
    first_light& out
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H
