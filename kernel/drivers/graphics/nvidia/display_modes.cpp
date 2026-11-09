#include "drivers/graphics/nvidia/display_modes.h"
#include "common/string.h"

namespace drivers::nvidia {

// Whether the order of preference ranks `first` before `second`: the one of more pixels, then of the faster refresh
static bool comes_before(const edid::timing& first, const edid::timing& second) {
    uint64_t first_pixels = static_cast<uint64_t>(first.width) * first.height;
    uint64_t second_pixels = static_cast<uint64_t>(second.width) * second.height;
    if (first_pixels != second_pixels) {
        return first_pixels > second_pixels;
    }

    return first.refresh_millihertz() > second.refresh_millihertz();
}

// Whether `timing` may be the output's mode: its link carries it, and on an output limited to 60 Hz it refreshes below
// the bound
static bool may_be_mode(const edid::timing& timing, const displayport::link_facts& facts, bool is_limited_to_60_hz) {
    if (is_limited_to_60_hz && timing.refresh_millihertz() >= LIMITED_REFRESH_BOUND_MILLIHERTZ) {
        return false;
    }

    displayport::link_configuration config = {};
    return displayport::find_lowest_configuration(facts, timing, config);
}

// Whether `list` holds a mode of `mode`'s width, height and refresh
static bool lists_mode(const mode_list& list, const gfxfb::gfxfb_mode& mode) {
    for (uint8_t index = 0; index < list.count; index++) {
        const gfxfb::gfxfb_mode& listed = list.modes[index];
        bool is_same_size = listed.width == mode.width && listed.height == mode.height;
        if (is_same_size && listed.refresh_millihertz == mode.refresh_millihertz) {
            return true;
        }
    }

    return false;
}

void find_mode_candidates(
    const edid::monitor& monitor,
    const displayport::link_facts& facts,
    bool is_limited_to_60_hz,
    mode_candidates& out
) {
    out = {};

    uint8_t first_ranked_slot = 0;
    for (uint32_t index = 0; index < monitor.timing_count; index++) {
        const edid::timing& timing = monitor.timings[index];
        if (!may_be_mode(timing, facts, is_limited_to_60_hz)) {
            continue;
        }

        if (index == 0 && monitor.first_timing_is_preferred) {
            out.timings[0] = 0;
            out.count = 1;
            first_ranked_slot = 1;
            continue;
        }

        // Each timing goes after every one it does not come before, so ties keep the EDID's order
        uint8_t slot = out.count;
        while (slot > first_ranked_slot && comes_before(timing, monitor.timings[out.timings[slot - 1]])) {
            out.timings[slot] = out.timings[slot - 1];
            slot--;
        }

        out.timings[slot] = static_cast<uint8_t>(index);
        out.count++;
    }
}

gfxfb::gfxfb_mode mode_of(const edid::timing& timing) {
    return {
        .width              = timing.width,
        .height             = timing.height,
        .refresh_millihertz = timing.refresh_millihertz(),
    };
}

void list_modes(const edid::timing* timings, uint8_t count, mode_list& out) {
    string::memset(&out, 0, sizeof(out));

    for (uint8_t index = 0; index < count; index++) {
        gfxfb::gfxfb_mode mode = mode_of(timings[index]);
        if (!lists_mode(out, mode)) {
            out.modes[out.count] = mode;
            out.count++;
        }
    }
}

} // namespace drivers::nvidia
