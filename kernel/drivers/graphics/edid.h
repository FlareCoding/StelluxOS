#ifndef STELLUX_DRIVERS_GRAPHICS_EDID_H
#define STELLUX_DRIVERS_GRAPHICS_EDID_H

#include "common/types.h"

/*
 * A monitor describes itself in its EDID, a series of 128-byte blocks that a
 * display driver reads from it over the cable through the driver's own device.
 * The first block, the base block, names the monitor's maker and product and
 * holds four 18-byte descriptors. Each one carries a timing (a video signal
 * the monitor accepts, described in full) or, as a display descriptor, text
 * such as the product name or facts the parser does not read. A preferred
 * timing is the one the monitor's maker recommends. The block's parts start at
 * these bytes:
 *
 *   byte 0     header pattern 00 FF FF FF FF FF FF 00
 *   byte 8     maker, then the product code, serial number and date
 *   byte 18    version, which must be 1, then the revision
 *   byte 20    input kind, screen size and gamma
 *   byte 24    features, bit 1 read as marking the first descriptor preferred
 *   byte 25    color points, then signals named by a bit or a code
 *   byte 54    four 18-byte descriptors, each a timing or a display descriptor
 *   byte 126   number of extension blocks after the base block
 *   byte 127   checksum, making the block's 128 bytes sum to 0 modulo 256
 *
 * The parser checks the block's size, header, checksum and version, then reads
 * the maker, the features byte and the descriptors. A signal named by a bit or
 * a code must be looked up in a table or computed, so it is not a timing and
 * the parser leaves it out.
 *
 * A display driver hands the bytes it read to `edid::parse`, which returns the
 * maker, the product name and the timings in an `edid::monitor`. The driver
 * chooses the timing it sends and must translate it into its own mode type
 * once, so no code past the driver uses the parser's types. The parser keeps
 * no state and never logs, allocates, blocks or touches hardware, so any
 * kernel thread may call it at any privilege.
 */

namespace edid {

constexpr int32_t OK               = 0;
constexpr int32_t ERR_TOO_SHORT    = -1; // fewer bytes than one block
constexpr int32_t ERR_BAD_HEADER   = -2; // the first block does not start with the EDID header pattern
constexpr int32_t ERR_BAD_CHECKSUM = -3; // the base block's bytes do not sum to 0 modulo 256
constexpr int32_t ERR_BAD_VERSION  = -4; // the base block's version is not 1

constexpr size_t   BLOCK_BYTES = 128;
constexpr uint32_t TIMINGS_MAX = 32; // the most timings one `monitor` holds

/**
 * One video signal a monitor accepts, described in full. Each line is sent as
 * its visible pixels, then a gap (the front porch), the horizontal sync pulse
 * and another gap (the back porch). Each frame is sent as its visible lines,
 * then a vertical front porch, sync pulse and back porch counted in lines.
 * Horizontal counts are pixels and vertical counts are lines. A sync pulse is
 * positive when the signal is high while the pulse lasts.
 */
struct timing {
    uint32_t pixel_clock_khz; // pixels sent per second, visible or not, in kHz
    uint32_t width;           // visible pixels per line
    uint32_t h_front_porch;
    uint32_t h_sync_width;
    uint32_t h_back_porch;
    uint32_t height;          // visible lines per frame
    uint32_t v_front_porch;
    uint32_t v_sync_width;
    uint32_t v_back_porch;
    bool     h_sync_positive;
    bool     v_sync_positive;

    uint32_t h_total() const; // pixels per line, visible or not
    uint32_t v_total() const; // lines per frame, visible or not

    // Frames per second in thousandths of a hertz, rounded down, 0 when either total is 0
    uint64_t refresh_millihertz() const;

    bool operator==(const timing& other) const = default;
};

/**
 * The maker, name and timings `parse` reads from a monitor's base block. A
 * `monitor` takes about 1.3 KiB, so a caller keeps it in its own state rather
 * than on the stack.
 */
struct monitor {
    char     maker[4];                  // the maker's three-letter Plug and Play id, such as "AOC"
    char     name[14];                  // the product name as `parse` reads it, empty when the block gives none
    timing   timings[TIMINGS_MAX];      // distinct timings in the order the EDID lists them
    uint32_t timing_count;              // entries of `timings` in use
    bool     first_timing_is_preferred; // `timings[0]` is the timing the monitor's maker recommends
};

/**
 * @brief Read a monitor's maker, name and timings from its EDID's base block.
 * The timings come from the timing descriptors in descriptor order, and one
 * equal in every field to a listed timing is not listed again. A descriptor
 * gives no timing when its pixel clock is 0 (the mark of a display
 * descriptor), when it starts with five 0x01 bytes (the fill of an unused
 * descriptor), when its width or height is 0, or when `timing` cannot describe
 * it: an interlaced timing (one sending every other line on each pass down the
 * screen), a timing with a border around its visible area, or a timing whose
 * front porch and sync pulse together are longer than the blanking (every
 * invisible pixel or line) on either axis. The first descriptor's timing is
 * the preferred one when it is listed and the features byte marks it, so a
 * preferred timing is always `timings[0]`. The features byte's mark is read
 * the same way at every revision.
 *
 * Polarities come from the descriptor's flags byte when its sync kind gives
 * them: separate digital sync gives both, digital composite sync only the
 * horizontal one and analog sync neither. A polarity the kind does not give
 * reads as negative for the horizontal pulse and positive for the vertical
 * one.
 *
 * The name is the text of the first product name descriptor whose reserved
 * byte 2 is 0: at most 13 characters, stopping before a newline or a NUL, and
 * without trailing spaces. A maker letter outside A to Z and a name byte
 * outside printable ASCII read as `?`.
 *
 * The caller owns `bytes` and `out`. The parser keeps no pointer to either and
 * no state between calls, so two threads may parse into two `monitor`s at
 * once, and a caller must keep a parse into `out` from overlapping any other
 * use of `out`.
 * @param bytes The EDID as read from the monitor, at any alignment. Only the
 * base block is read, so bytes past it are ignored.
 * @param size Number of bytes at `bytes`.
 * @param out Rewritten whole. On an error every field is zero. On OK, `maker`
 * and `name` end in a NUL, `timing_count` is at most `TIMINGS_MAX`, and every
 * listed timing has a nonzero pixel clock, width and height. `out` may list no
 * timings, as for a base block holding only display descriptors.
 * @return OK, or the first base block check that fails, in this order:
 * ERR_TOO_SHORT, ERR_BAD_HEADER, ERR_BAD_CHECKSUM, ERR_BAD_VERSION. A read
 * over the cable may have corrupted bytes, so ERR_BAD_CHECKSUM is worth one
 * more read.
 */
int32_t parse(const uint8_t* bytes, size_t size, monitor& out);

} // namespace edid

#endif // STELLUX_DRIVERS_GRAPHICS_EDID_H
