// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_WINDOW_OWNERS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_WINDOW_OWNERS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// NVIDIA's NVC67D_WINDOW_SET_CONTROL_OWNER_NONE, the owner of a window that no head owns. Heads are the units that each
// generate one monitor's timing, and windows the units that each scan one image out onto a head
constexpr uint8_t WINDOW_OWNER_NONE = 0xF;

/**
 * Each window's owner, the head the window scans its image out onto, as the
 * core channel's armed state holds it. The armed state holds what the display
 * engine took at its last update. An update that changes a window's owner must
 * not be interlocked with (take effect together with) an update of the
 * window's own channel, as NVIDIA's modesetting module notes.
 */
struct window_owners {
    uint8_t owners[DISPLAY_WINDOWS_MAX]; // entry i for window i, its control's 4-bit owner field
};

/**
 * @brief Read the owner of each present window from the core channel's armed
 * state. NVIDIA's modesetting module reads the owners this way before the
 * first update since it allocated the core channel, the update that sets every
 * window's owner, so that it can leave each window whose owner changes out of
 * that update's interlock. That module reads them after its wait for the core
 * channel to go idle, whether or not the firmware reported the channel idle,
 * so a caller must call this after `wait_core_channel_idle`. The windows read
 * are those the firmware reports present, where that module reads the usable
 * windows the display engine's capabilities give, which the driver does not
 * read.
 * @param bar0 The card's BAR0 registers, in which the core channel's area
 * lies.
 * @param present_windows The windows `read_present_windows` gave, bit i for
 * window i.
 * @return For each window `present_windows` names, entry i holding the 4-bit
 * owner field of window i's control, kept as read, and 0 in every other entry.
 */
window_owners read_window_owners(const registers& bar0, uint32_t present_windows);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_WINDOW_OWNERS_H
