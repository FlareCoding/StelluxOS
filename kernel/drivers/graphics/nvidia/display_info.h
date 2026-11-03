// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_INFO_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_INFO_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * @brief Ask the GSP's firmware for the display's static information (fixed
 * facts about the display engine) and give the heads, among the first 8, and
 * the windows it reports present on this GPU, bit i for head or window i. A
 * window mask naming a window at or past the chip's `window_count` is refused,
 * so every window it names is one the chip table describes. Neither mask
 * changes on an error.
 * @return `OK`, what `firmware::internal_control` returns, logged as it
 * describes, or `ERR_BAD_MESSAGE`, logged, for a window the chip lacks.
 */
int32_t read_present_heads_and_windows(
    firmware& fw,
    const chip_display& display,
    uint32_t& present_heads,
    uint32_t& present_windows
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_INFO_H
