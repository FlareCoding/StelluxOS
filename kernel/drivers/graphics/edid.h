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
 * the maker, the features byte, the descriptors and byte 126. A signal named
 * by a bit or a code must be looked up in a table or computed, so it is not a
 * timing and the parser leaves it out.
 *
 * Extension blocks follow the base block, as many as its byte 126 says, each
 * 128 bytes with the same checksum rule. An extension block's first byte, its
 * tag, names its kind, such as 0x02 for CTA-861 (the extension TVs and many
 * monitors add) or 0x70 for DisplayID (a separate format whose sections an
 * EDID can carry). A CTA-861 block can hold more timing descriptors. A
 * DisplayID block holds one section of DisplayID data with a checksum byte of
 * its own, and a version 1 section can hold Type I descriptors, DisplayID 1's
 * 20-byte timing descriptors. The parser lists the timings of both after the
 * base block's, and no block of another kind gives a timing. The parser counts
 * as skipped each extension block it is not given whole, each CTA-861 or
 * DisplayID block failing its checksum and each DisplayID block whose section
 * does not end before the block's checksum byte or fails its own checksum, so
 * a driver can tell that the EDID arrived cut short, corrupted or malformed. A
 * skipped block gives no timing, and a block of another kind is skipped only
 * when it is not given whole.
 *
 * A display driver hands the bytes it read to `edid::parse`, which returns the
 * maker, the product name, the timings, the number of timings that did not fit
 * and the number of skipped extension blocks in an `edid::monitor`. The driver
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
 * The maker, name and timings `parse` reads from a monitor's EDID, how many
 * timings did not fit and how many extension blocks `parse` skipped. A
 * `monitor` takes about 1.3 KiB, so a caller keeps it in its own state rather
 * than on the stack.
 */
struct monitor {
    char     maker[4];                  // the maker's three-letter Plug and Play id, such as "AOC"
    char     name[14];                  // the product name as `parse` reads it, empty when the block gives none
    timing   timings[TIMINGS_MAX];      // distinct timings in the order the EDID lists them
    uint32_t timing_count;              // entries of `timings` in use
    bool     first_timing_is_preferred; // `timings[0]` is the timing the monitor's maker recommends
    uint32_t dropped_timings;           // timings found with `timings` full that equal no listed timing
    uint32_t skipped_extensions;        // extension blocks cut short, and CTA-861 or DisplayID ones failing a check
};

/**
 * @brief Read a monitor's maker, name and timings from its EDID, and count the
 * timings that did not fit and the skipped extension blocks.
 * The timings come from the base block's timing descriptors in descriptor
 * order, then from the extension blocks in block order, and one equal in every
 * field to a listed timing is not listed again. When `timings` is full, a
 * timing equal to no listed one is counted in `dropped_timings` instead, each
 * time it is found, so a caller can tell that `out` lacks a timing. One equal
 * to a listed timing is not counted, as `out` lists it. A descriptor gives no
 * timing when `timing` cannot describe it: an interlaced timing (one sending
 * every other line on each pass down the screen), a timing with a border
 * around its visible area, or a timing whose front porch and sync pulse
 * together are longer than the blanking (every invisible pixel or line) on
 * either axis. An 18-byte descriptor also gives none when its pixel clock is 0
 * (the mark of a display descriptor), when it starts with five 0x01 bytes (the
 * fill of an unused descriptor), or when its width or height is 0. The timing
 * of the base block's first descriptor is the preferred one when it is listed
 * and the features byte marks it, so a preferred timing is always
 * `timings[0]`. The features byte's mark is read the same way at every
 * revision.
 *
 * An 18-byte descriptor's polarities come from its flags byte when its sync
 * kind gives them: separate digital sync gives both, digital composite sync
 * only the horizontal one and analog sync neither. A polarity the kind does
 * not give reads as negative for the horizontal pulse and positive for the
 * vertical one. A Type I descriptor gives both, each axis's in the top bit of
 * its front porch.
 *
 * The name is the text of the first product name descriptor whose reserved
 * byte 2 is 0: at most 13 characters, stopping before a newline or a NUL, and
 * without trailing spaces. A maker letter outside A to Z and a name byte
 * outside printable ASCII read as `?`.
 *
 * The base block's byte 126 says how many extension blocks follow it. Each of
 * them is skipped when `bytes` does not hold it whole, when it is a CTA-861 or
 * DisplayID block whose 128 bytes do not sum to 0 modulo 256, or when it is a
 * DisplayID block whose section fails the checks below, and
 * `skipped_extensions` counts the skipped blocks. A block of another kind is
 * skipped only when `bytes` does not hold it whole. A skipped block gives no
 * timing and never makes the call fail, so it never costs the caller the
 * timings of the other blocks.
 *
 * A CTA-861 block's bytes 0 to 3 are its tag, its revision, the offset of its
 * first timing descriptor and a byte the parser does not read. The bytes from
 * byte 4 up to that offset hold facts the parser does not read either, among
 * them signals named by a code. The parser reads descriptors from that offset
 * up to the first whose pixel clock is 0 or that would not end before the
 * checksum byte 127, so at most six. A block at revision 0, or whose byte 2 is
 * below 4 (0 meaning no descriptors, 1 to 3 an offset within bytes 0 to 3),
 * gives no timings.
 *
 * A DisplayID block holds one section from byte 1. Bytes 1 to 4 are the
 * section's version byte, the byte count of its data blocks (the tagged
 * records describing the monitor), a product type and a count of extension
 * sections. The product type is a code for the kind of product the section
 * describes, and the count says how many sections extend this one. The data
 * blocks follow from byte 5, then a checksum byte making the section's bytes
 * sum to 0 modulo 256. The bytes after that checksum byte, up to the block's
 * own, are not part of the section. The section fails at any version when its
 * data blocks take more than 121 bytes, which puts its checksum byte at or
 * past the block's, or when its bytes do not sum to 0. Only a section of
 * version 1 (a version byte of 0x10 to 0x1F) gives timings, whatever its
 * product type and count, and one of any other version that passes the checks
 * gives none and is not counted. Each data block is a tag, a revision, its
 * length (the count of bytes that follow) and those bytes. The parser walks
 * the data blocks in order, listing the timings of each Type I timing block
 * (tag 3) and passing over the others, and stops at a data block that would
 * not end before the section's checksum byte or at the zero padding after the
 * last data block. The padding is any tag 0 block but a first one holding
 * bytes (the product identification block). A Type I block holds 20-byte
 * descriptors and gives no timings when its length is not a multiple of 20.
 *
 * The caller owns `bytes` and `out`. The parser keeps no pointer to either and
 * no state between calls, so two threads may parse into two `monitor`s at
 * once, and a caller must keep a parse into `out` from overlapping any other
 * use of `out`.
 * @param bytes The EDID as read from the monitor, at any alignment. Only the
 * base block and the extension blocks its byte 126 says follow it are read, so
 * bytes past them are ignored.
 * @param size Number of bytes at `bytes`.
 * @param out Rewritten whole. On an error every field is zero. On OK, `maker`
 * and `name` end in a NUL, `timing_count` is at most `TIMINGS_MAX`, every
 * listed timing has a nonzero pixel clock, width and height, `dropped_timings`
 * is 0 unless `timing_count` is `TIMINGS_MAX`, and `skipped_extensions` is at
 * most the number in byte 126. `out` may list no timings, as for a base block
 * holding only display descriptors and followed by no CTA-861 or DisplayID
 * block.
 * @return OK, or the first base block check that fails, in this order:
 * ERR_TOO_SHORT, ERR_BAD_HEADER, ERR_BAD_CHECKSUM, ERR_BAD_VERSION. A read
 * over the cable may have corrupted bytes, so ERR_BAD_CHECKSUM is worth one
 * more read.
 */
int32_t parse(const uint8_t* bytes, size_t size, monitor& out);

} // namespace edid

#endif // STELLUX_DRIVERS_GRAPHICS_EDID_H
