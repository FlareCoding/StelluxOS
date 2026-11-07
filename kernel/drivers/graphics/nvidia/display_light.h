// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/gfxfb.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_edid.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// The bytes of NVIDIA's look-up table surface, its NVEvoLutDataRec's size rounded up to a multiple of 64: a window's
// input table of 1029 8-byte entries, then a head's output table of as many, each padded to the next multiple of 256
constexpr uint64_t LOOK_UP_TABLE_SURFACE_BYTES = 0x4200;

// NVIDIA's NV_LUT_VSS_HEADER_SIZE and NV_NUM_EVO_LUT_ENTRIES: a look-up table's header entries, then its entries, and
// their sum, the SIZE NVIDIA writes in a table's control
constexpr uint32_t LOOK_UP_TABLE_HEADER_ENTRIES = 4;
constexpr uint32_t LOOK_UP_TABLE_ENTRIES        = 1025;
constexpr uint32_t LOOK_UP_TABLE_SIZE           = LOOK_UP_TABLE_HEADER_ENTRIES + LOOK_UP_TABLE_ENTRIES;

// Where the head's output table begins in the look-up table surface, NVEvoLutDataRec's output, 256-byte aligned
constexpr uint64_t OUTPUT_TABLE_OFFSET = 0x2100;

// NVIDIA's NV_EVO_SCALE_FACTOR_1X, a scale factor of one in 1024ths
constexpr uint32_t SCALE_FACTOR_ONE = 1024;

// The pixels a window fetches a line beyond its width when it does not scale, the worst case NVIDIA's modesetting
// module takes on every chip, (((width + 14) * 1024 + 1023) >> 10) + 8
constexpr uint32_t PIXELS_FETCHED_BEYOND_WIDTH = 22;

// The multiple NVIDIA's modesetting module requires of the pitch of a surface laid out as rows of pixels in order
constexpr uint32_t SURFACE_PITCH_ALIGNMENT = 256;

/**
 * A connected output's modes, the timings of its EDID that may be its mode in
 * the order `find_mode_candidates` ranks them, the DPCD capabilities they
 * were ranked under, the EDID's bytes and the monitor they name.
 */
struct output_modes {
    uint8_t           count;
    uint8_t           indices[edid::TIMINGS_MAX]; // each mode's index in the EDID, the first `count` entries
    edid::timing      timings[edid::TIMINGS_MAX]; // each mode's timing, the first `count` entries
    dpcd_capabilities dpcd;
    edid_copy         edid;

    gfxfb::monitor_identity monitor; // zero when the EDID does not parse
};

/**
 * The head and window to show the output to light first, and the outputs tried
 * first.
 */
struct light_target {
    uint8_t  head;
    uint8_t  window;
    uint32_t first_outputs; // a bit for each output tried before the others, bit 0 output 0's
};

/**
 * An output to light, the mode to light it at and the head and window to show
 * it.
 */
struct output_light {
    uint8_t      output;
    uint8_t      timing_index; // the mode's index in the output's EDID
    edid::timing timing;
    uint8_t      head;
    uint8_t      window;
};

/**
 * The outputs one mode set lights, each on its own head, in lighting order:
 * the target's first outputs, then the others, each in ascending order.
 */
struct light_set {
    uint8_t      count;
    output_light lights[DISPLAY_HEADS_MAX];
};

/**
 * A head's raster for a timing, as NVIDIA's modesetting module derives it, its
 * positions counting pixels and lines from the start of the sync. The sync end
 * is the last pixel and line of the sync, the blanking end the last pixel and
 * line of the back porch, and the blanking start the last pixel and line of
 * the visible area. Each value takes 32 bits, so a sync end with no sync, or a
 * blanking end with no sync or back porch before it, is 0xFFFFFFFF, where that
 * module's 16-bit positions give 0xFFFF. The idle lines are those that module
 * counts before and after a frame's visible lines when the head does not
 * scale: the sync's and back porch's before, the front porch's after.
 */
struct head_raster {
    uint32_t width;  // the pixels of a line, the timing's total
    uint32_t height; // the lines of a frame, the timing's total
    uint32_t blank_start_x;
    uint32_t blank_start_y;
    uint32_t blank_end_x;
    uint32_t blank_end_y;
    uint32_t sync_end_x;
    uint32_t sync_end_y;
    uint32_t leading_idle_lines;
    uint32_t trailing_idle_lines;
};

/**
 * @brief Give the head and window to show the output to light first, the boot
 * head, else head 0, with that head's lowest window, and the outputs tried
 * first, those an active head shows.
 * @param heads The heads `read_display_heads` gave.
 * @param active The heads `read_active_heads` gave for `heads.count` heads.
 * @param outputs The outputs `read_display_outputs` gave.
 * @return The head, below `heads.count`, the window, and the outputs tried
 * first, each below `outputs.count`.
 */
light_target find_light_target(const display_heads& heads, const active_heads& active, const display_outputs& outputs);

/**
 * @brief Give a head's raster for a progressive timing.
 * @param timing The timing, whose totals must fit 32 bits.
 * @return The raster.
 */
head_raster raster_of(const edid::timing& timing);

/**
 * @brief Give the pitch of a base buffer, the bytes from the start of one row
 * of its image to the next: `width` pixels of 4 bytes (the size of a pixel in
 * the formats the bandwidth check is asked about), rounded up to a multiple of
 * 256 bytes, since NVIDIA's modesetting module refuses to register, with any
 * other pitch, a surface laid out as rows of pixels in order.
 * @param width The image's width in pixels, at most 65,536, the widest an EDID
 * timing gives (DisplayID's 16-bit width field plus 1).
 * @return The pitch in bytes.
 */
uint32_t base_buffer_pitch(uint32_t width);

/**
 * @brief Give the 8-byte word at `index` of the look-up table surface, as
 * `display_engine::set_up` writes it. The surface's first 1029 entries are
 * the window's input table: 4 header entries, where a table of segments keeps
 * their sizes, then 1025 entries holding NVIDIA's identity, each but the last
 * holding its place among them over 1024 as a half-precision float in red,
 * green and blue, 16 bits each from bit 0, and the last repeating the one
 * before. The head's output table, at `OUTPUT_TABLE_OFFSET`, is laid out the
 * same way and holds NVIDIA's output identity, each entry but the last holding
 * its place among them as a 10-bit level in the top 10 bits of 16 in red,
 * green and blue, and the last repeating the one before. Each entry's top 16
 * bits, which NVIDIA names unused, are 0, and so is every other word: the
 * header entries, the padding that places the output table at byte 8448 and
 * ends NVEvoLutDataRec at `LOOK_UP_TABLE_SURFACE_BYTES`, and the words past
 * that the allocation's whole pages add.
 * @param index The word's place in the surface's memory.
 * @return The word.
 */
uint64_t look_up_table_word(uint32_t index);

/**
 * @brief Choose the outputs one mode set lights, in lighting order, up to
 * `heads.count`, each at the first of its modes the bandwidth check accepts
 * beside the outputs chosen before it, the first on the target's head and
 * window and each later one on the lowest head left with its lowest window. An
 * output refused at every mode or left without a head is not chosen, logged.
 * @param target The head, window and first outputs `find_light_target` gave.
 * @return `OK` with `out` holding the outputs chosen, `ERR_NO_MEMORY` when the
 * request cannot be allocated, or the error of a request failing other than by
 * a refusal, logged, which ends the choice with `out` empty.
 */
int32_t choose_lights(
    firmware& fw,
    rm::handle bandwidth_check,
    const display_heads& heads,
    const output_modes* modes,
    uint8_t output_count,
    const light_target& target,
    light_set& out
);

/**
 * @brief Choose the mode `output` lights at on `head` with `window` while the
 * outputs of `lit` stay lit at theirs: the first of its modes the bandwidth
 * check accepts beside them, as `choose_lights` asks about a later output.
 * @param lit The lit outputs, each on a head other than `head`.
 * @return `OK` with `is_chosen` set and `out` holding the light when a mode is
 * accepted, `OK` with `is_chosen` clear when every mode is refused, each
 * refusal logged, `ERR_NO_MEMORY` when the request cannot be allocated, or the
 * error of a request failing other than by a refusal, both logged.
 */
int32_t choose_light(
    firmware& fw,
    rm::handle bandwidth_check,
    uint8_t output,
    const output_modes& modes,
    const light_set& lit,
    uint8_t head,
    uint8_t window,
    output_light& out,
    bool& is_chosen
);

/**
 * @brief Ask the bandwidth check once whether every head of `set` can show its
 * output's mode at once, as `choose_lights` asks, for a set that lost an
 * output after the choice. `set` must hold lights `choose_lights` chose.
 * @return `OK` with `is_possible` set to the answer, false after a refused
 * request, `ERR_NO_MEMORY` when the request cannot be allocated, or the error
 * of a request failing other than by a refusal, each logged.
 */
int32_t check_light_set(firmware& fw, rm::handle bandwidth_check, const light_set& set, bool& is_possible);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H
