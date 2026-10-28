#include "drivers/graphics/nvidia/display_light.h"

namespace drivers::nvidia {

// The output tried after `tried` others: `first_output` first, then the others in ascending order
static uint8_t output_in_order(uint8_t tried, uint8_t first_output) {
    if (tried == 0) {
        return first_output;
    }

    return tried <= first_output ? tried - 1 : tried;
}

light_target find_light_target(const display_heads& heads, const active_heads& active, const display_outputs& outputs) {
    light_target target = {};
    if (find_boot_head(active, target.head)) {
        find_shown_output(active, target.head, outputs, target.first_output);
    }

    // The boot head and head 0 are below `heads.count`, and `display_heads` promises each such head a window
    target.window = static_cast<uint8_t>(__builtin_ctz(windows_of_head(heads, target.head)));

    return target;
}

bool choose_first_light(
    const output_modes* modes,
    uint8_t output_count,
    const light_target& target,
    first_light& out
) {
    for (uint8_t tried = 0; tried < output_count; tried++) {
        uint8_t output = output_in_order(tried, target.first_output);
        if (modes[output].count == 0) {
            continue;
        }

        out = {
            .output = output,
            .timing_index = modes[output].indices[0],
            .timing = modes[output].timings[0],
            .head = target.head,
            .window = target.window,
        };

        return true;
    }

    return false;
}

} // namespace drivers::nvidia
