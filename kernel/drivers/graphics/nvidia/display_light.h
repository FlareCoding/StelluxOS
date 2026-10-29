// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H

#include "common/types.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// The bytes of NVIDIA's look-up table surface, its NVEvoLutDataRec's size rounded up to a multiple of 64: a window's
// input table of 1029 8-byte entries, then a head's output table of as many, each padded to the next multiple of 256
constexpr uint64_t LOOK_UP_TABLE_SURFACE_BYTES = 0x4200;

// NVIDIA's NV_LUT_VSS_HEADER_SIZE and NV_NUM_EVO_LUT_ENTRIES: a look-up table's header entries, then its entries
constexpr uint32_t LOOK_UP_TABLE_HEADER_ENTRIES = 4;
constexpr uint32_t LOOK_UP_TABLE_ENTRIES        = 1025;

/**
 * A connected output's modes, the timings of its EDID that may be its mode in
 * the order `find_mode_candidates` ranks them, and the DPCD capabilities they
 * were ranked under.
 */
struct output_modes {
    uint8_t           count;
    uint8_t           indices[edid::TIMINGS_MAX]; // each mode's index in the EDID, the first `count` entries
    edid::timing      timings[edid::TIMINGS_MAX]; // each mode's timing, the first `count` entries
    dpcd_capabilities dpcd;
};

/**
 * The head and window to show the output to light first, and the output tried
 * first.
 */
struct light_target {
    uint8_t head;
    uint8_t window;
    uint8_t first_output;
};

/**
 * The output to light first, the mode to light it at and the head and window
 * to show it.
 */
struct first_light {
    uint8_t      output;
    uint8_t      timing_index; // the mode's index in the output's EDID
    edid::timing timing;
    uint8_t      head;
    uint8_t      window;
};

/**
 * A head's raster for a timing, as NVIDIA's modesetting module derives it, its
 * positions counting pixels and lines from the start of the sync. The blanking
 * end is the last pixel and line of the back porch, and the blanking start the
 * last pixel and line of the visible area. Each value takes 32 bits, so a
 * blanking end with no sync or back porch before it is 0xFFFFFFFF, where that
 * module's 16-bit positions give 0xFFFF.
 */
struct head_raster {
    uint32_t width;  // the pixels of a line, the timing's total
    uint32_t height; // the lines of a frame, the timing's total
    uint32_t blank_start_x;
    uint32_t blank_start_y;
    uint32_t blank_end_x;
    uint32_t blank_end_y;
};

/**
 * @brief Give the head and window to show the output to light first, the boot
 * head, else head 0, with that head's lowest window, and the output tried
 * first, the boot output, else output 0.
 * @param heads The heads `read_display_heads` gave.
 * @param active The heads `read_active_heads` gave for `heads.count` heads.
 * @param outputs The outputs `read_display_outputs` gave.
 * @return The head, below `heads.count`, the window, and the output tried
 * first, below `outputs.count` unless that is 0.
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
 * before. Each entry's top 16 bits, which NVIDIA names unused, are 0, and so
 * is every other word: the header entries, the output table, which a head
 * could read only through a binding to the core channel, the padding that
 * places the output table at byte 8448 and ends NVEvoLutDataRec at
 * `LOOK_UP_TABLE_SURFACE_BYTES`, and the words past that the allocation's
 * whole pages add. NVIDIA's default table, the one a window with no table of
 * its own reads, holds NVIDIA's output identity in its output table as well.
 * @param index The word's place in the surface's memory.
 * @return The word.
 */
uint64_t look_up_table_word(uint32_t index);

/**
 * @brief Choose the output to light first and its mode, asking the bandwidth
 * check whether the target's head can show each mode with the target's window
 * scanning out an image of the mode's size, 4 bytes a pixel and unscaled. The
 * outputs are tried from the target's first output, then the others in
 * ascending order, each output's modes in order, and the first mode the check
 * accepts is chosen. The request names only that head and window, with the
 * look-up tables and the 256-pixel cursor NVIDIA's modesetting module assumes,
 * where that module names every window of every lit head and lowers what it
 * asks of them, finally below 4 bytes a pixel, before refusing a mode, so a
 * mode it accepts only below 4 bytes a pixel is refused here. A mode whose
 * request the firmware refuses with `ERR_FIRMWARE` is refused, as in that
 * module, and so is a mode whose sync and back porch take fewer than 2 lines,
 * without asking the check, as that module refuses a mode of 1 line but asks
 * about one of 0 lines. Each refusal is logged.
 * @param fw The driver's firmware.
 * @param bandwidth_check The handle `allocate_display_objects` gave the
 * bandwidth check's object.
 * @param modes The modes of each of the `output_count` outputs.
 * @param output_count How many outputs `modes` holds.
 * @param target The head, window and first output `find_light_target` gave,
 * whose first output must be below `output_count` unless that is 0.
 * @param out Receives the choice when one is made, and is left unchanged
 * otherwise.
 * @param is_chosen Receives on `OK` whether a mode was chosen.
 * @return `OK`, `ERR_NO_MEMORY` when the request cannot be allocated on the
 * unprivileged heap, or what `firmware::control` returns for a request that
 * fails other than with `ERR_FIRMWARE`, logged, which ends the choice, where
 * NVIDIA's modesetting module refuses the mode.
 */
int32_t choose_first_light(
    firmware& fw,
    rm::handle bandwidth_check,
    const output_modes* modes,
    uint8_t output_count,
    const light_target& target,
    first_light& out,
    bool& is_chosen
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_LIGHT_H
