// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_MODES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_MODES_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/gfxfb.h"
#include "drivers/graphics/nvidia/displayport.h"

namespace drivers::nvidia {

// The refresh a timing must stay below on an output limited to 60 Hz: 101% of 60 Hz, as NVIDIA's modesetting module
// allows a refresh less than 1% above the top of a range of refresh rates
constexpr uint64_t LIMITED_REFRESH_BOUND_MILLIHERTZ = 60600;

static_assert(edid::TIMINGS_MAX <= gfxfb::GFXFB_MODES_MAX, "an output lists at most GFXFB_MODES_MAX modes");

/**
 * The timings of an output's EDID that may be its mode, in the order
 * `find_mode_candidates` ranks them, the first being the mode.
 */
struct mode_candidates {
    uint8_t count;
    uint8_t timings[edid::TIMINGS_MAX]; // distinct indices below the EDID's `timing_count`, the first `count` entries
};

// The modes an output lists while lit, in the order `list_modes` gives them, each with the first timing giving it
struct mode_list {
    uint8_t           count;
    gfxfb::gfxfb_mode modes[edid::TIMINGS_MAX];          // distinct modes, the first `count` entries
    uint8_t           timing_indices[edid::TIMINGS_MAX]; // each mode's timing's index in the EDID
    edid::timing      timings[edid::TIMINGS_MAX];        // each mode's timing, at which its request lights the output
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

// The mode an output lit at `timing` reads at: the timing's visible width, height and refresh
gfxfb::gfxfb_mode mode_of(const edid::timing& timing);

/**
 * @brief Give the modes of `count` timings in their order, each as `mode_of`
 * gives it with the timing and EDID index it came from, leaving out a mode an
 * earlier timing gave, as for two timings whose sync polarities alone differ.
 * @param indices Each timing's index in the EDID.
 * @param timings The timings to list.
 * @param count How many timings to list, which must be at most
 * `edid::TIMINGS_MAX`.
 * @param out Rewritten whole.
 */
void list_modes(const uint8_t* indices, const edid::timing* timings, uint8_t count, mode_list& out);

// Whether two modes have the same width, height and refresh
bool is_same_mode(const gfxfb::gfxfb_mode& a, const gfxfb::gfxfb_mode& b);

// Whether `list` holds `mode`, setting `rank` to its place when it does
bool find_listed_mode(const mode_list& list, const gfxfb::gfxfb_mode& mode, uint8_t& rank);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_MODES_H
