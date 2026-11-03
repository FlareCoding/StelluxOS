// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_METHODS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_METHODS_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_link.h"
#include "drivers/graphics/nvidia/rm.h"

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
 * @param present_windows The windows `read_present_heads_and_windows` gave,
 * bit i for window i.
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

// What one head is lit with: its mode, the display id it shows, the serial link it feeds, the windows it serves and the
// look-up table surface holding its output table
struct head_light {
    uint8_t      head;
    uint32_t     windows;                  // bit i for window i, each window serving `head`
    edid::timing timing;                   // a progressive timing whose raster has 2 or more leading idle lines
    uint32_t     display_id;               // the display id the head shows, a single bit
    output_link  link;                     // the serial link the head feeds and the sublink the display id takes
    rm::handle   output_table_context_dma; // names the look-up table surface, bound to the core channel
};

// The most methods `build_head_light_methods` gives: 50 for the head and its link and one for each window it serves
constexpr uint32_t HEAD_LIGHT_FIXED_METHODS = 50;
constexpr uint32_t HEAD_LIGHT_METHODS_MAX   = HEAD_LIGHT_FIXED_METHODS + DISPLAY_WINDOWS_MAX;

// The core channel's methods lighting one head, the first `count` of `methods` in the order they are written
struct head_light_methods {
    display_method methods[HEAD_LIGHT_METHODS_MAX];
    uint32_t       count;
};

// The methods `build_window_usage_methods` gives: the formats, input scale factor and usage bounds of one window
constexpr uint32_t WINDOW_USAGE_METHODS = 3;

// The core channel's methods bounding what a window scans out for its first image
struct window_usage_methods {
    display_method methods[WINDOW_USAGE_METHODS];
};

/**
 * @brief Give the core channel's methods that light one head at its mode
 * through its serial link, ahead of the update that applies them, in the order
 * NVIDIA's modesetting module writes them for a head it lights with RGB at 8
 * bits a color, at full range and with no scaling, compression, stereo or
 * lock. First the head's output table (a window's input table goes with its
 * image instead), NVIDIA's output identity at `OUTPUT_TABLE_OFFSET` of the
 * surface `output_table_context_dma` names, read directly by a color's top 10
 * bits with no interpolation or mirroring at NVIDIA's norm scale outside HDR,
 * then OCSC0, the first of the head's two output color space conversion (CSC)
 * matrices, turned on, which NVIDIA does only after the table. Then the
 * raster: a black overscan, the totals, sync end, blanking end and start and
 * idle lines `raster_of` gives, the pixel clock as both its frequency and its
 * highest frequency, with no hopping between frequencies, and no frame packing
 * color, HDMI video format, tile or blanking delay. Then the head's control
 * written whole, at 0, no flip lock pin, lock chain or lock offset and no
 * compression. Then the output resource's control, which takes a CRC over the
 * whole raster, the mode's sync polarities, 24 bits a pixel and no packet
 * window. Then RGB at full range, the OCSC0 matrix NVIDIA computes for RGB at
 * the default saturation (the identity, each row adding 1/2048 for NVIDIA's
 * output rounding) and no output CSC (OCSC1) matrix. Then the serial link set
 * to carry the head alone in DisplayPort on the display id's sublink, and the
 * head's display id. Then a viewport the size of the mode and unscaled, each
 * window serving the head given NVIDIA's default usage bounds for a line of
 * the mode's width, the head's own usage bounds as NVIDIA's mode set writes
 * them for that viewport, which allow its output table, and the viewport's
 * origin at 0, 0. Last the output scaler's two taps. Unlike that module, which
 * also writes the head's dither, these methods leave it as it is, and they
 * write the color controls once where that module writes them again for its
 * digital vibrance. That module turns off the table's interpolation and adds
 * the 1/2048 only with its output rounding fix on, as these methods always do.
 * @param light The head, mode, display id, link, windows and output table's
 * context DMA, which must be nonzero, name the look-up table surface and be
 * bound to the core channel. The head must be below the chip's heads, the
 * link's index below its serial links and its sublink 0 or 1.
 * @param out Receives the methods.
 */
void build_head_light_methods(const head_light& light, head_light_methods& out);

/**
 * @brief Give the core channel's methods that bound what a window scans out
 * for its first image, as NVIDIA's modesetting module writes them with the
 * image: the formats it may scan, an input scale factor of one both ways and
 * NVIDIA's default usage bounds for a line of its head's viewport. Where that
 * module allows each format the window's usage names, these allow only those
 * of 4 bytes a pixel, the formats the bandwidth check is asked about.
 * @param window The window, below the chip's windows.
 * @param viewport_width The width of the viewport of the head the window
 * serves, the mode's width when the head does not scale.
 * @param out Receives the methods.
 */
void build_window_usage_methods(uint8_t window, uint32_t viewport_width, window_usage_methods& out);

// The methods `build_cursor_methods` gives: present control, both eyes' context DMAs and offsets, control, composition
constexpr uint32_t CURSOR_METHODS = 7;

// The core channel's methods choosing what a head's cursor shows
struct cursor_methods {
    display_method methods[CURSOR_METHODS];
};

/**
 * @brief Give the core channel's methods that show a head's cursor from the
 * 256 by 256 picture at the start of the memory `context_dma` names, blended
 * by its straight alpha, or that show no cursor when `context_dma` is 0.
 * @param head The head, below the chip's heads.
 * @param context_dma Names the picture's memory, bound to the core channel,
 * or 0 for no cursor.
 * @param out Receives the methods.
 */
void build_cursor_methods(uint8_t head, rm::handle context_dma, cursor_methods& out);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_CORE_METHODS_H
