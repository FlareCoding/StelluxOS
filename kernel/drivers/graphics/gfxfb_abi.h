#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H

/*
 * Userland draws on the screen through /dev/gfxfb, and this file defines the
 * device's ioctl numbers, constants and argument structures for the kernel and
 * userland alike. The kernel implements the calls and includes the file
 * through gfxfb.h, and userland includes the copy libstlx installs as
 * <stlx/gfxfb_abi.h>. The two take the fixed-width integer types from
 * different headers, so the file includes nothing and an includer must provide
 * those types first.
 *
 * A program reads the layout of the device's pixels with `GFXFB_GET_INFO`,
 * maps the pixels with `mmap` at offset 0 and draws into the mapping. When the
 * layout's `flags` hold `GFXFB_INFO_NEEDS_FLUSH`, the screen shows the drawing
 * only once `GFXFB_FLUSH` names the regions that changed. Otherwise the
 * mapping holds the pixels the screen shows.
 *
 * The device drives outputs, numbered from 0, each of which can show one
 * monitor's picture. `GFXFB_GET_OUTPUT` reads one output's state: whether it
 * is lit (showing a picture), the mode it is lit at (its picture size and
 * refresh rate), and whether it is the primary output, whose pixels
 * `GFXFB_GET_INFO` describes, offset 0 maps and `GFXFB_FLUSH` shows. While no
 * output is lit there is no primary output, so `GFXFB_GET_INFO` and
 * `GFXFB_FLUSH` fail with EAGAIN and mmap at offset 0 is refused.
 *
 * The kernel and userland are built from one tree, so a structure may gain
 * fields when its call gains a feature. Each structure holds fixed-width
 * fields with explicit padding, so both builds lay it out alike on x86_64 and
 * AArch64, and its size check at the end of the file fails any build that
 * sizes the structure differently.
 */

namespace gfxfb {

constexpr uint32_t GFXFB_GET_INFO   = 0x4700;
constexpr uint32_t GFXFB_FLUSH      = 0x4701;
constexpr uint32_t GFXFB_GET_OUTPUT = 0x4702;

constexpr uint8_t GFXFB_INFO_NEEDS_FLUSH = 0x01; // drawing reaches the screen only through GFXFB_FLUSH

constexpr uint32_t GFXFB_MAX_FLUSH_RECTS = 256;

constexpr uint32_t GFXFB_OUTPUT_LIT     = 0x1; // the output shows a picture
constexpr uint32_t GFXFB_OUTPUT_PRIMARY = 0x2; // the output whose pixels offset 0 maps

// GFXFB_GET_INFO argument, filled with the layout of the pixels mapped at offset 0
struct gfxfb_info {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t  flags; // GFXFB_INFO_*
    uint8_t  padding[2];
    uint64_t size;
};

struct gfxfb_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

// GFXFB_FLUSH argument, naming the regions of the mapping that changed
struct gfxfb_flush_args {
    uint64_t rects;    // user address of `count` gfxfb_rect entries
    uint32_t count;    // at most GFXFB_MAX_FLUSH_RECTS
    uint32_t reserved; // must be zero
};

// The picture size and refresh rate an output is lit at
struct gfxfb_mode {
    uint32_t width;
    uint32_t height;
    uint64_t refresh_millihertz; // 0 when unknown
};

// GFXFB_GET_OUTPUT argument, filled with one output's state
struct gfxfb_output_info {
    uint32_t   output;       // in: the output's index, EINVAL unless below `output_count`
    uint32_t   output_count; // out: how many outputs the device drives
    uint32_t   flags;        // out: GFXFB_OUTPUT_*
    uint32_t   reserved;     // out: zero
    gfxfb_mode current;      // out: the mode while lit, zero while dark
};

static_assert(sizeof(gfxfb_info) == 40, "gfxfb_info layout");
static_assert(sizeof(gfxfb_rect) == 16, "gfxfb_rect layout");
static_assert(sizeof(gfxfb_flush_args) == 16, "gfxfb_flush_args layout");
static_assert(sizeof(gfxfb_mode) == 16, "gfxfb_mode layout");
static_assert(sizeof(gfxfb_output_info) == 32, "gfxfb_output_info layout");

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H
