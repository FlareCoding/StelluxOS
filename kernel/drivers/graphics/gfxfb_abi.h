#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H

/*
 * Userland draws on the screen through /dev/gfxfb, and this file defines the
 * device's ioctl numbers, constants, argument structures and event record for
 * the kernel and userland alike. The kernel implements the calls and includes
 * the file through gfxfb.h, and userland includes the copy libstlx installs as
 * <stlx/gfxfb_abi.h>. The two take the fixed-width integer types from
 * different headers, so the file includes nothing and an includer must provide
 * those types first.
 *
 * A program reads the layout of the device's pixels with `GFXFB_GET_INFO`,
 * maps the pixels with `mmap` at offset 0 and draws into the mapping. When the
 * layout's `flags` hold `GFXFB_INFO_NEEDS_FLUSH`, the screen shows the drawing
 * only once `GFXFB_FLUSH` names the regions that changed. Otherwise the
 * mapping holds the pixels the screen shows. Either way, a present of a
 * created buffer on the primary output, as described below, replaces the
 * drawing once it latches, until a present of handle 0 latches there.
 * `GFXFB_FLUSH` fails with EINVAL when the kernel cannot read its argument, a
 * `gfxfb_flush_args`, when the argument's reserved field is set or when it
 * names more than `GFXFB_MAX_FLUSH_RECTS` rects, and otherwise with EIO once
 * the kernel has found the device failed, even while no output is lit. While a
 * created buffer shows on the primary output and the output holds no present
 * of handle 0, a flush that passes those checks succeeds without reading its
 * rects or reaching the screen, and the next present of handle 0 there that
 * the device accepts shows the whole drawing. Past those checks, any other
 * flush fails with EAGAIN when there is no primary output, before the kernel
 * reads its rects, and otherwise with EINVAL when the kernel cannot read one
 * of its rects, with EAGAIN when the primary output is dark and with EIO when
 * the device refuses the copy or has failed.
 *
 * The device drives outputs, numbered from 0, each of which can show one
 * monitor's picture. `GFXFB_GET_OUTPUT` reads one output's state: whether it
 * is lit (showing a picture), the mode it is lit at (its picture size and
 * refresh rate), and whether it is the primary output, whose pixels
 * `GFXFB_GET_INFO` describes, offset 0 maps and `GFXFB_FLUSH` shows. While no
 * output is lit there is no primary output, so `GFXFB_GET_INFO` fails with
 * EAGAIN, `GFXFB_FLUSH` fails with EAGAIN unless the kernel has found the
 * device failed, and mmap at offset 0 is refused. A device that had no memory
 * to record its lit outputs' pixels as it started has no primary output
 * either.
 *
 * A program can also create buffers, each holding one picture of 32-bit
 * pixels. `GFXFB_CREATE_BUFFER` returns a new buffer's handle, its layout and
 * the mmap offset that maps it. An open file controls the buffers from its
 * first `GFXFB_CREATE_BUFFER` or `GFXFB_PRESENT` that succeeds until it
 * closes. A creation fails with EINVAL when the kernel cannot read its
 * argument, when its width or height is zero or when its reserved field is
 * set. Any other creation, and any present that passes the checks below, fails
 * with EBUSY while another open file controls the buffers or reserves control,
 * and otherwise reserves control for its file until the call returns, so two
 * files never control the buffers at once. A creation that reserves control
 * fails with ENOTTY on a device that cannot create buffers, with ENOMEM when
 * no memory is left or the file already holds `GFXFB_CLIENT_BUFFERS_MAX`
 * handles, with EIO when the device refuses the buffer or has failed, and with
 * EINVAL when the kernel cannot write its argument after the device created
 * the buffer. A handle names its buffer only within the open file that created
 * it. `GFXFB_DESTROY_BUFFER` drops a handle, failing with EINVAL for one the
 * file does not hold, and closing the file drops all of them. A buffer stays
 * allocated until its handle and its last mapping are gone and no output shows
 * it or holds it for a present.
 *
 * `GFXFB_PRESENT` asks for a buffer to replace what an output shows from the
 * output's next vertical blank and returns without waiting for it. The present
 * latches at the vertical blank from which the output shows the buffer. Handle
 * 0 names an output's base buffer, the pixels the output was lit with. The
 * output shows them until a present of another buffer latches and again once a
 * present of handle 0 latches, and on the primary output they are the pixels
 * offset 0 maps. A present may name the regions of its buffer that changed
 * since the buffer was created or last presented. A device that shows a copy
 * of each buffer copies only the regions named, so pixels changed outside them
 * may not reach the screen, and copies the whole buffer for a present naming
 * no region, whose `regions` address the kernel never reads, and for the first
 * present of handle 0 on the primary output that it accepts after a successful
 * `GFXFB_FLUSH` that did not reach the screen, whatever regions the present
 * names, since that flush copied none. A present fails with EINVAL when the
 * kernel cannot read its argument, when its reserved field is set or it names
 * more than `GFXFB_MAX_FLUSH_RECTS` regions, for an output of `output_count`
 * or more and for a handle the file does not hold, and otherwise with EAGAIN
 * for handle 0 when the device started with the output dark or with no memory
 * to record its lit outputs' pixels. It then fails with ENOMEM when no memory
 * is left to copy its regions, and otherwise with EINVAL when the kernel
 * cannot read them or one of them has a zero width or height or does not lie
 * wholly inside the buffer. Past those checks and control, it fails with EIO
 * once the kernel has found the device failed, and otherwise with EBUSY while
 * the output holds an earlier present, which it does until that present
 * latches or the output goes dark, or while the file holds the event of its
 * last present on the output. Otherwise it fails with the first that applies
 * of ENOTTY on a device that cannot present, EIO on a device that failed,
 * EAGAIN on a dark output, EINVAL when the buffer's width or height differs
 * from the output's mode, and EIO when the device refused the buffer.
 *
 * Each present the device accepted ends once: it latches, or the output goes
 * dark or the device fails before it latches. The open file that presented
 * learns of the end through an event, a `gfxfb_event` record that `read`
 * returns. Since an output holds one present at a time and a file's present on
 * an output fails while the file holds the event of its last one there, a file
 * holds at most one event per output. A read of size 0 returns 0, and one with
 * a null buffer fails with EFAULT at once. Any other read of a file opened
 * write-only fails with EBADF, so such a file never reads its events: once a
 * present of it on an output has ended, `poll` reports it readable for good
 * and its later presents there fail for good. Otherwise a read whose size
 * holds less than one record fails with EINVAL, and one holding a record or
 * more returns as many of the file's events as fit, oldest first, as whole
 * records. With none queued, it fails at once with EAGAIN on a non-blocking
 * file, and otherwise waits for one, which only the end of one of the file's
 * presents brings, so with none of them pending only a signal ends the wait. A
 * signal handled during the wait fails the read with EINTR, or restarts it
 * once the handler returns when the handler has SA_RESTART. A read that cannot
 * write the events it took to its buffer fails with EFAULT, and those events
 * are lost. `readv` reads its buffers in turn, each as a read of its own
 * except that a null one is read as a buffer that cannot be written instead of
 * failing at once, and stops after the first that fails or returns less than
 * its size, returning the bytes read by then, or the failing read's error when
 * there are none. `poll` reports the file readable while it holds an event,
 * and never writable. A file that closes while a present of it has not ended
 * waits for that end, whose event then reaches no file. A closing file that
 * controls the buffers then drops the events it holds and presents the base
 * buffer of each output showing another buffer, as a present of handle 0
 * naming no region. It also waits for the ends of those the device accepts,
 * and their events reach no file either. It keeps control through both waits.
 * An output where the base buffer's present does not latch, as on a dark
 * output or a failed device, keeps showing the buffer it showed.
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
constexpr uint32_t GFXFB_PRESENT        = 0x4705;

constexpr uint8_t GFXFB_INFO_NEEDS_FLUSH = 0x01; // drawing reaches the screen only through GFXFB_FLUSH

constexpr uint32_t GFXFB_MAX_FLUSH_RECTS = 256; // the most regions one flush or present names

constexpr uint32_t GFXFB_OUTPUTS_MAX = 16; // the most outputs a device drives

constexpr uint32_t GFXFB_OUTPUT_LIT     = 0x1; // the output shows a picture
constexpr uint32_t GFXFB_OUTPUT_PRIMARY = 0x2; // the output whose pixels offset 0 maps

// How a present the device accepted ended
constexpr uint32_t GFXFB_PRESENT_LATCHED = 0; // the present latched, so the output shows its buffer
constexpr uint32_t GFXFB_PRESENT_DARK    = 1; // the output went dark before the present latched
constexpr uint32_t GFXFB_PRESENT_FAILED  = 2; // the device failed before the present latched

constexpr uint32_t GFXFB_EVENT_PRESENT = 1; // a `gfxfb_event` reporting how a present ended

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

// GFXFB_PRESENT argument, naming an output, the buffer asked to replace what it shows from its next vertical blank and
// the regions of that buffer that changed since it was created or last presented
struct gfxfb_present_args {
    uint32_t output;       // the output's index, EINVAL unless below `output_count`
    uint32_t handle;       // the buffer's handle, or 0 for the output's base buffer
    uint64_t regions;      // user address of `region_count` gfxfb_rect entries
    uint32_t region_count; // at most GFXFB_MAX_FLUSH_RECTS, or 0 for the whole buffer
    uint32_t reserved;     // must be zero
};

// One event as `read` returns it
struct gfxfb_event {
    uint32_t type;         // GFXFB_EVENT_PRESENT
    uint32_t output;       // the output the present was for
    uint32_t status;       // GFXFB_PRESENT_*, how the present ended
    uint32_t handle;       // the handle the present named, 0 for the output's base buffer
    uint64_t frame;        // the output's vertical blanks since it was lit, counted at the latch, 0 unless latched
    uint64_t timestamp_ns; // when the latching vertical blank began, on the kernel's monotonic clock, 0 unless latched
};

static_assert(sizeof(gfxfb_info) == 40, "gfxfb_info layout");
static_assert(sizeof(gfxfb_rect) == 16, "gfxfb_rect layout");
static_assert(sizeof(gfxfb_flush_args) == 16, "gfxfb_flush_args layout");
static_assert(sizeof(gfxfb_mode) == 16, "gfxfb_mode layout");
static_assert(sizeof(gfxfb_output_info) == 32, "gfxfb_output_info layout");
static_assert(sizeof(gfxfb_buffer_args) == 40, "gfxfb_buffer_args layout");
static_assert(sizeof(gfxfb_present_args) == 24, "gfxfb_present_args layout");
static_assert(sizeof(gfxfb_event) == 32, "gfxfb_event layout");

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H
