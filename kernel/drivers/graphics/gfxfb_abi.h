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
 * the kernel has found the device failed, even while there is no primary
 * output or it relights, and then with EAGAIN when there is no primary output
 * or the primary output relights, before the kernel reads its rects. While a
 * created buffer shows on the primary output and the output holds no present
 * of handle 0, a flush that passes those checks succeeds without reading its
 * rects or reaching the screen, and the next present of handle 0 there that
 * the device accepts shows the whole drawing. Past those checks, any other
 * flush fails with EINVAL when the kernel cannot read one of its rects, with
 * EAGAIN when the device finds the primary output dark and with EIO when the
 * device refuses the copy or has failed.
 *
 * The device drives outputs, numbered from 0, each of which can show one
 * monitor's picture. `GFXFB_GET_OUTPUT` reads one output's state: whether it
 * is lit (showing a picture), the mode it is lit at (its picture size and
 * refresh rate), and whether it is the primary output, whose pixels
 * `GFXFB_GET_INFO` describes, offset 0 maps and `GFXFB_FLUSH` shows. The
 * device may light an output, take it dark, or relight it at another mode by
 * taking it dark and lighting it again, and each change reaches every open
 * file as an output event, as described below. The primary stays on an output
 * that relights, which reads primary but not lit meanwhile. A primary output
 * that goes dark otherwise hands the primary to the lit output with the lowest
 * number, and with no primary output the first output to light takes it. An
 * output whose pixels the kernel had no memory to record as it lit counts as
 * dark for these rules, as do the lit outputs of a device that had no memory
 * to record their pixels as it started. An output that gains the primary also
 * gets an output event. The kernel moves the primary and records or drops an
 * output's pixels as it queues the output event of the change, while
 * `GFXFB_GET_OUTPUT` asks the device and may show a change before its output
 * event. While there is no primary output or it relights, `GFXFB_GET_INFO`
 * fails with EAGAIN, `GFXFB_FLUSH` fails with EAGAIN unless the kernel has
 * found the device failed, and mmap at offset 0 is refused. When the primary
 * moves or the primary output relights, the primary output may have other
 * pixels at another size, so a program reads `GFXFB_GET_INFO` and maps offset
 * 0 again after an output event, while its earlier mapping keeps the pixels it
 * mapped, which no output shows any more.
 *
 * A program can also create buffers, each holding one picture of 32-bit
 * pixels. `GFXFB_CREATE_BUFFER` returns a new buffer's handle, its layout and
 * the mmap offset that maps it. An open file controls the buffers from its
 * first creation, present or cursor call that succeeds until it closes. A
 * creation fails with EINVAL when the kernel cannot read its argument, when
 * its width or height is zero or when its reserved field is set. Any other
 * creation, and any present or cursor call that passes the checks below, fails
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
 * for handle 0 while the kernel has recorded no pixels for the output. It then
 * fails with ENOMEM when no memory is left to copy its regions, and otherwise
 * with EINVAL when the kernel cannot read them or one of them has a zero width
 * or height or does not lie wholly inside the buffer. Past those checks and
 * control, it fails with EIO once the kernel has found the device failed, and
 * otherwise with EBUSY while the output holds an earlier present, which it
 * does until that present latches or the output goes dark, or while the file
 * holds the event of its last present on the output. Otherwise it fails with
 * the first that applies of ENOTTY on a device that cannot present, EIO on a
 * device that failed, EAGAIN on a dark output, EINVAL when the buffer's width
 * or height differs from the output's mode, and EIO when the device refused
 * the buffer.
 *
 * A device with no vertical blanks counts boundaries of its own at the
 * output's refresh rate in their place, and may show a presented buffer before
 * the boundary its present latches at.
 *
 * Each present the device accepted ends once: it latches, or the output goes
 * dark or the device fails before it latches. The open file that presented
 * learns of the end through a present event, a `gfxfb_event` record that
 * `read` returns. Since an output holds one present at a time and a file's
 * present on an output fails while the file holds the event of its last one
 * there, a file holds at most one present event per output. Until the kernel
 * finds the device failed, each change of an output queues an output event
 * naming it in every open file, behind the file's other events and in place of
 * an output event the file holds for that output, so a file holds at most one
 * output event per output and reads a present's dark end before the output
 * event of its output going dark. A program reads the output's new state with
 * `GFXFB_GET_OUTPUT`. A read of size 0 returns 0, and one with a null buffer
 * fails with EFAULT at once. Any other read of a file opened write-only fails
 * with EBADF, so such a file never reads its events: once it holds an event,
 * `poll` reports it readable for good, and once a present of it on an output
 * has ended, its later presents there fail for good. Otherwise a read whose
 * size holds less than one record fails with EINVAL, and one holding a record
 * or more returns as many of the file's events as fit, oldest first, as whole
 * records. With none queued, it fails at once with EAGAIN on a non-blocking
 * file, and otherwise waits for one, which the end of one of the file's
 * presents or a change of an output brings, or for a signal, so with no
 * present pending on a device that never reports a change it waits for a
 * signal. A signal handled during the wait fails the read with EINTR, or
 * restarts it once the handler returns when the handler has SA_RESTART. A read
 * that cannot write the events it took to its buffer fails with EFAULT, and
 * those events are lost. `readv` reads its buffers in turn, each as a read of
 * its own except that a null one is read as a buffer that cannot be written
 * instead of failing at once, and stops after the first that fails or returns
 * less than its size, returning the bytes read by then, or the failing read's
 * error when there are none. `poll` reports the file readable while it holds
 * an event, and never writable. A file that closes while a present of it has
 * not ended waits for that end, whose event then reaches no file. A closing
 * file that controls the buffers then drops the events it holds, presents the
 * base buffer of each output showing another buffer, as a present of handle 0
 * naming no region, and hides each output's cursor. It also waits for the ends
 * of those presents the device accepts, and their events reach no file either.
 * It keeps control through both waits. An output where the base buffer's
 * present does not latch keeps showing the buffer it showed, as on a failed
 * device, unless it went dark, since a dark output shows no buffer. An output
 * keeps showing its cursor when the device refuses the hide or the kernel has
 * found the device failed.
 *
 * An output may have a hardware cursor, a small picture the device draws over
 * what the output shows, so moving it changes no buffer. `GFXFB_GET_OUTPUT`
 * reports the largest cursor picture an output shows, 0 by 0 for an output
 * without a hardware cursor. Two cursor calls drive it. `GFXFB_SET_CURSOR`
 * gives an output's cursor a picture and its hotspot (the picture's pixel that
 * sits at the cursor's position), or hides the cursor when the picture's width
 * is 0, reading none of the picture's other fields. Each pixel holds alpha in
 * bits 31 to 24 and red, green and blue as a buffer's pixel does, none
 * multiplied by alpha. The kernel copies the picture, so the program may
 * change its pixels once the call returns. `GFXFB_MOVE_CURSOR` places the
 * hotspot of an output's cursor at a position in the output's pixels, even one
 * off the output, so the picture may lie partly or wholly off it. An output
 * that goes dark loses its cursor, which stays hidden once the output lights
 * again until the next picture. A move on a lit output while its cursor is
 * hidden, before any picture, after a hide or since the output last went dark,
 * only records the position, where the next picture shows. A cursor call fails
 * with EINVAL when the kernel cannot read its argument, when its reserved
 * field is set and for an output of `output_count` or more. A
 * `GFXFB_SET_CURSOR` showing a picture then fails with EINVAL when its hotspot
 * lies outside the picture or the picture is wider or taller than the output's
 * largest, with ENOMEM when no memory is left to copy the picture, and
 * otherwise with EINVAL when the kernel cannot read its pixels. Past those
 * checks and control, a cursor call fails with EIO once the kernel has found
 * the device failed, and otherwise with the first that applies of ENOTTY on an
 * output without a hardware cursor, EIO on a device that failed, EAGAIN on a
 * dark output, EINVAL for a picture wider or taller than the output's largest,
 * and EIO when the device refused the call.
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
constexpr uint32_t GFXFB_SET_CURSOR     = 0x4706;
constexpr uint32_t GFXFB_MOVE_CURSOR    = 0x4707;

constexpr uint8_t GFXFB_INFO_NEEDS_FLUSH = 0x01; // drawing reaches the screen only through GFXFB_FLUSH

constexpr uint32_t GFXFB_MAX_FLUSH_RECTS = 256; // the most regions one flush or present names

constexpr uint32_t GFXFB_OUTPUTS_MAX = 16; // the most outputs a device drives

constexpr uint32_t GFXFB_OUTPUT_LIT     = 0x1; // the output shows a picture
constexpr uint32_t GFXFB_OUTPUT_PRIMARY = 0x2; // offset 0 maps its pixels while lit, set without LIT as it relights

// How a present the device accepted ended
constexpr uint32_t GFXFB_PRESENT_LATCHED = 0; // the present latched, so the output shows its buffer
constexpr uint32_t GFXFB_PRESENT_DARK    = 1; // the output went dark before the present latched
constexpr uint32_t GFXFB_PRESENT_FAILED  = 2; // the device failed before the present latched

// A device ends each present it accepted at the latest this long after GFXFB_PRESENT returns, not counting time the
// kernel spends waiting for the CPU
constexpr uint64_t GFXFB_PRESENT_TIMEOUT_NS = 100000000;

constexpr uint32_t GFXFB_EVENT_PRESENT = 1; // a `gfxfb_event` reporting how a present ended
constexpr uint32_t GFXFB_EVENT_OUTPUT  = 2; // a `gfxfb_event` naming an output whose state changed

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
    uint32_t   output;            // in: the output's index, EINVAL unless below `output_count`
    uint32_t   output_count;      // out: how many outputs the device drives
    uint32_t   flags;             // out: GFXFB_OUTPUT_*
    uint32_t   reserved;          // out: zero
    gfxfb_mode current;           // out: the mode while lit, zero while dark
    uint32_t   cursor_max_width;  // out: the widest cursor picture the output shows, 0 without a hardware cursor
    uint32_t   cursor_max_height; // out: the tallest, 0 without a hardware cursor
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

// GFXFB_SET_CURSOR argument, giving an output's hardware cursor a picture and its hotspot, or hiding the cursor
struct gfxfb_cursor_args {
    uint32_t output;   // the output's index, EINVAL unless below `output_count`
    uint32_t width;    // at most the output's `cursor_max_width`, or 0 to hide the cursor
    uint32_t height;   // at most the output's `cursor_max_height`
    uint32_t hot_x;    // the hotspot's column in the picture, below `width`
    uint32_t hot_y;    // the hotspot's row in the picture, below `height`
    uint32_t reserved; // must be zero
    uint64_t pixels;   // user address of `width` times `height` pixels, row after row with no padding
};

// GFXFB_MOVE_CURSOR argument, placing the hotspot of an output's cursor
struct gfxfb_cursor_position {
    uint32_t output;   // the output's index, EINVAL unless below `output_count`
    int32_t  x;        // the hotspot's column in the output's pixels, negative left of the output
    int32_t  y;        // the hotspot's row, negative above the output
    uint32_t reserved; // must be zero
};

// One event as `read` returns it. An output event holds its type and output, with every other field zero
struct gfxfb_event {
    uint32_t type;         // GFXFB_EVENT_*
    uint32_t output;       // the output the present was for, or the output that changed
    uint32_t status;       // GFXFB_PRESENT_* in a present event, how the present ended
    uint32_t handle;       // the handle the present named, 0 for the output's base buffer
    uint64_t frame;        // the output's vertical blanks since it was lit, counted at the latch, 0 unless latched
    uint64_t timestamp_ns; // when the latching vertical blank began, on the kernel's monotonic clock, 0 unless latched
};

static_assert(sizeof(gfxfb_info) == 40, "gfxfb_info layout");
static_assert(sizeof(gfxfb_rect) == 16, "gfxfb_rect layout");
static_assert(sizeof(gfxfb_flush_args) == 16, "gfxfb_flush_args layout");
static_assert(sizeof(gfxfb_mode) == 16, "gfxfb_mode layout");
static_assert(sizeof(gfxfb_output_info) == 40, "gfxfb_output_info layout");
static_assert(sizeof(gfxfb_buffer_args) == 40, "gfxfb_buffer_args layout");
static_assert(sizeof(gfxfb_present_args) == 24, "gfxfb_present_args layout");
static_assert(sizeof(gfxfb_cursor_args) == 32, "gfxfb_cursor_args layout");
static_assert(sizeof(gfxfb_cursor_position) == 16, "gfxfb_cursor_position layout");
static_assert(sizeof(gfxfb_event) == 32, "gfxfb_event layout");

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_ABI_H
