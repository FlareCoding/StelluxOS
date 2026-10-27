// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_HEADS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_HEADS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware.h"

namespace drivers::nvidia {

/**
 * The heads (the units that each generate one monitor's timing) display
 * common's report leaves the display engine, numbered from 0, and the head
 * each window (the units that each scan one image out onto a head) serves.
 * Each head below `count` is served by at least one window, and no window
 * serves a head at or past it.
 */
struct display_heads {
    uint8_t count;
    uint8_t window_head[DISPLAY_WINDOWS_MAX]; // the head window i serves, or `NO_HEAD`
};

// The head `display_heads` records for a window that serves none, a number no head takes
constexpr uint8_t NO_HEAD = 0xFF;

/**
 * @brief Ask display common how many heads the display engine can use, the
 * mask of those heads and the heads each window may serve, in that order and
 * each request carrying its parameters zeroed, and give the heads and each
 * window's head by the rule of NVIDIA's modesetting module. When window 0's
 * mask is `rm::FLEXIBLE_WINDOW_HEADS` the mapping is flexible, and windows 2N
 * and 2N + 1 serve head N. Otherwise each window serves the one head its mask
 * names. A window whose mask is 0 serves none. The heads the report leaves the
 * display engine are those the head mask names that some window serves,
 * counted from head 0 up to the first one missing and to no more than the
 * count, and a window whose head is past them then serves none. Refused as
 * malformed, with no later request sent, are a count of 0, as NVIDIA's module
 * refuses it, and, where NVIDIA's module goes on, a report that leaves the
 * display engine no head, a window's mask naming several heads in a mapping
 * that is not flexible, and a report past the chip table's entry: a count
 * above its `head_count`, a head mask naming a head at or past its
 * `head_count`, a window's mask doing so in a mapping that is not flexible, or
 * a nonzero mask for a window at or past its `window_count`.
 * @param fw The driver's firmware.
 * @param display_common The handle `allocate_display_objects` gave display
 * common.
 * @param display The chip table's display entry for this GPU's chip.
 * @param out Receives on `OK` the heads, at least one and at most the chip's
 * `head_count`, with no window at or past the chip's `window_count` serving
 * one, and is left unchanged on an error.
 * @return `OK`, what `firmware::control` returns for a request, logged as its
 * doc describes, such as the `ERR_FIRMWARE` of a window request the firmware
 * refuses as not supported, where NVIDIA's module takes the head mask as the
 * heads with windows, or `ERR_BAD_MESSAGE`, logged here, for a report refused
 * as malformed.
 */
int32_t read_display_heads(firmware& fw, rm::handle display_common, const chip_display& display, display_heads& out);

/**
 * @brief Give the windows that serve a head.
 * @param heads The heads `read_display_heads` gave.
 * @param head The head asked about.
 * @return The windows that serve `head`, bit i for window i, or 0 for a head
 * at or past `heads.count`.
 */
uint32_t windows_of_head(const display_heads& heads, uint8_t head);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_HEADS_H
