// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_INFO_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_INFO_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * @brief Ask the GSP's firmware for the display's static information (fixed
 * facts about the display engine, such as its windows) and give the mask of
 * the windows it reports present, those of the chip's windows this GPU has.
 * NVIDIA's driver asks for it when it initializes its display's state, and
 * later allows a window's channel (the queue of commands that sets up what the
 * window scans out) only for a window the mask names. The request carries the
 * information's 28 bytes, zeroed, through `firmware::internal_control`.
 * NVIDIA's driver keeps the mask unchecked, while here a mask naming a window
 * at or past the chip's `window_count` is refused, so a caller can take every
 * window the mask names as one the chip table describes.
 * @param fw The driver's firmware.
 * @param display The chip table's display entry for this GPU's chip.
 * @param present_windows Receives on `OK` the mask of present windows, bit i
 * for window i, each below `display.window_count`. It is left unchanged on an
 * error.
 * @return `OK`, what `firmware::internal_control` returns, logged as its doc
 * describes, or `ERR_BAD_MESSAGE`, logged here, when the mask names a window
 * the chip lacks.
 */
int32_t read_present_windows(firmware& fw, const chip_display& display, uint32_t& present_windows);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_INFO_H
