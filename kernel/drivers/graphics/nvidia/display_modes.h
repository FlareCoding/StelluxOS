// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_MODES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_MODES_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/displayport.h"

namespace drivers::nvidia {

// The refresh a timing must stay below on an output limited to 60 Hz: 101% of 60 Hz, as NVIDIA's modesetting module
// allows a refresh less than 1% above the top of a range of refresh rates
constexpr uint64_t LIMITED_REFRESH_BOUND_MILLIHERTZ = 60600;

/**
 * The timings of an output's EDID that may be its mode, in the order
 * `find_mode_candidates` ranks them, the first being the mode.
 */
struct mode_candidates {
    uint8_t count;
    uint8_t timings[edid::TIMINGS_MAX]; // distinct indices below the EDID's `timing_count`, the first `count` entries
};

/**
 * @brief Give the timings of an output's EDID that may be its mode, in the
 * driver's order of preference: the preferred timing first when the EDID
 * marks one, then the others by pixel count (width times height), then by
 * refresh, both descending, ties in the EDID's order. A timing may be the
 * mode only when `displayport::find_lowest_configuration` finds a link
 * configuration carrying it, so none may when the GPU's links lack FEC
 * support. On an output limited to 60 Hz a timing must also refresh below
 * `LIMITED_REFRESH_BOUND_MILLIHERTZ`, which keeps timings slower than 60 Hz
 * too, though NVIDIA documents the firmware's limit as allowing 60 Hz modes
 * only. NVIDIA's modesetting module reads no 60 Hz limit.
 * @param monitor The output's EDID as `edid::parse` gave it.
 * @param facts The output's link facts.
 * @param is_limited_to_60_hz Whether the output is limited to 60 Hz.
 * @param out Rewritten whole, with a count of 0 when no timing may be the
 * mode.
 */
void find_mode_candidates(
    const edid::monitor& monitor,
    const displayport::link_facts& facts,
    bool is_limited_to_60_hz,
    mode_candidates& out
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_MODES_H
