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
 * `GFXFB_FLUSH` fail with EAGAIN and mmap at offset 0 is refused. A device
 * that had no memory to record its primary output's pixels as it started has
 * no primary output either.
 *
 * A program can also create buffers, each holding one picture of 32-bit
 * pixels. `GFXFB_CREATE_BUFFER` returns a new buffer's handle, its layout and
 * the mmap offset that maps it. It fails with ENOSYS on a device that cannot
 * create buffers, with ENOMEM when no memory is left or the file already holds
 * `GFXFB_CLIENT_BUFFERS_MAX` handles, and with EIO when the device fails. A
 * handle names its buffer only within the open file that created it.
 * `GFXFB_DESTROY_BUFFER` drops a handle, failing with EINVAL for one the file
 * does not hold, and closing the file drops all of them. A buffer stays
 * allocated until its handle and its last mapping are gone.
 *
 * The kernel and userland are built from one tree, so a structure may gain
 * fields when its call gains a feature. Each structure holds fixed-width
 * fields with explicit padding, so both builds lay it out alike on x86_64 and
 * AArch64, and its size check at the end of the file fails any build that
 * sizes the structure differently.
 */

namespace gfxfb {

constexpr uint32_t GFXFB_GET_INFO       = 0x4700;
constexpr uint32_t GFXFB_FLUSH          = 0x4701;
constexpr uint32_t GFXFB_GET_OUTPUT     = 0x4702;
constexpr uint32_t GFXFB_CREATE_BUFFER  = 0x4703;
constexpr uint32_t GFXFB_DESTROY_BUFFER = 0x4704; // the argument is the handle itself

constexpr uint8_t GFXFB_INFO_NEEDS_FLUSH = 0x01; // drawing reaches the screen only through GFXFB_FLUSH

constexpr uint32_t GFXFB_MAX_FLUSH_RECTS = 256;

constexpr uint32_t GFXFB_OUTPUT_LIT     = 0x1; // the output shows a picture
constexpr uint32_t GFXFB_OUTPUT_PRIMARY = 0x2; // the output whose pixels offset 0 maps

constexpr uint32_t GFXFB_FORMAT_XRGB8888 = 1; // red in bits 23 to 16, green 15 to 8, blue 7 to 0, 31 to 24 unused

constexpr uint32_t GFXFB_CLIENT_BUFFERS_MAX = 32; // the handles one open file can hold at once

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

// GFXFB_CREATE_BUFFER argument, filled with the new buffer's handle, layout and mmap offset
struct gfxfb_buffer_args {
    uint32_t width;      // in: EINVAL when zero or past the device's limit
    uint32_t height;     // in: EINVAL when zero or past the device's limit
    uint32_t format;     // in: GFXFB_FORMAT_XRGB8888, EINVAL for any other
    uint32_t handle;     // out
    uint32_t pitch;      // out: bytes per row
    uint32_t reserved;   // must be zero
    uint64_t size;       // out: bytes to map
    uint64_t map_offset; // out: the mmap offset that maps this buffer
};

static_assert(sizeof(gfxfb_info) == 40, "gfxfb_info layout");
static_assert(sizeof(gfxfb_rect) == 16, "gfxfb_rect layout");
static_assert(sizeof(gfxfb_flush_args) == 16, "gfxfb_flush_args layout");
static_assert(sizeof(gfxfb_mode) == 16, "gfxfb_mode layout");
static_assert(sizeof(gfxfb_output_info) == 32, "gfxfb_output_info layout");
static_assert(sizeof(gfxfb_buffer_args) == 40, "gfxfb_buffer_args layout");

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H
