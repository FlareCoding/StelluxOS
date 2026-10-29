// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_METHODS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_METHODS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_heads.h"

namespace drivers::nvidia {

// One method for a display channel: a register offset of the channel's class and the one value written there
struct display_method {
    uint32_t method;
    uint32_t value;
};

// NVIDIA's coefficients for each head's output scaler, the filter that resizes what the head sends out: 4 values for
// each of 16 phases (positions between two source pixels) of 3 ratios (1, 2 and 4, the last two shrinking the image)
constexpr uint32_t SCALER_RATIOS       = 3;
constexpr uint32_t SCALER_PHASES       = 16;
constexpr uint32_t SCALER_PHASE_VALUES = 4;
constexpr uint32_t SCALER_COEFFICIENTS = SCALER_RATIOS * SCALER_PHASES * SCALER_PHASE_VALUES;

// The methods `core_initial_method` gives for each head: one for each scaler coefficient, then two setting its desktop
// color, the color the head shows where no window covers it
constexpr uint32_t DESKTOP_COLOR_METHODS         = 2;
constexpr uint32_t CORE_INITIAL_METHODS_PER_HEAD = SCALER_COEFFICIENTS + DESKTOP_COLOR_METHODS;

/**
 * @brief Give one of the core channel's initial methods, which NVIDIA's
 * modesetting module writes into the core channel right after allocating it.
 * The core channel is the display channel that sets up heads, links and which
 * head owns each window. In NVIDIA's order, each scaler coefficient in turn is
 * loaded into every head, from head 0, with the coefficient's position in
 * NVIDIA's table as its index. Then every head's desktop color is set to
 * opaque black in two methods. The methods take effect only at an update, a
 * later method of the core channel that applies those written before it.
 * @param head_count How many heads `read_display_heads` leaves the display
 * engine.
 * @param index Which method, counted from 0 in the order above.
 * @param out Receives the method when one is given, and is left unchanged
 * otherwise.
 * @return True, with `out` given the method, when `index` is below
 * `CORE_INITIAL_METHODS_PER_HEAD` times `head_count`, and false otherwise.
 */
bool core_initial_method(uint8_t head_count, uint32_t index, display_method& out);

// The most methods `build_shutdown_methods` gives: a control and usage bounds for each window, then four for each head
constexpr uint32_t SHUTDOWN_METHODS_MAX = DISPLAY_WINDOWS_MAX * 2 + DISPLAY_HEADS_MAX * 4;

// The core channel's methods of a shutdown update, the first `count` of `methods` in the order they are written
struct shutdown_methods {
    display_method methods[SHUTDOWN_METHODS_MAX];
    uint32_t       count;
};

/**
 * @brief Give the core channel's methods that stop every active head, ahead of
 * the update that applies them, in the order NVIDIA's modesetting module
 * writes them at its first mode set after boot. When `maps_windows` is set,
 * since the update is the first since the core channel's allocation, each
 * present window first gets its head, or none, before any gets NVIDIA's
 * default usage bounds. Then each active head in ascending order has its
 * cursor disabled with no context DMA (part of NVIDIA's blank of a head), the
 * serial link it feeds set to carry no head, and its display id cleared. A
 * link two active heads share is set twice, each time to carry no head, where
 * NVIDIA's first write keeps the other head: the update applies the last
 * write, which carries no head in NVIDIA's sequence as in the driver's.
 * @param heads The heads `read_display_heads` gave, with the head each window
 * serves.
 * @param present_windows The windows `read_present_windows` gave, bit i for
 * window i.
 * @param active The heads `read_active_heads` gave, each active one's link a
 * serial link of the chip's.
 * @param maps_windows Whether the update is the first since the core channel's
 * allocation, so the windows' heads and usage bounds go first.
 * @param out Receives the methods.
 */
void build_shutdown_methods(
    const display_heads& heads,
    uint32_t present_windows,
    const active_heads& active,
    bool maps_windows,
    shutdown_methods& out
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_METHODS_H
