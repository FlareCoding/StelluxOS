#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_H

#include "common/types.h"
#include "drivers/graphics/gfxfb_abi.h"
#include "mm/pmm_types.h"

/*
 * gfxfb gives userland the screen through /dev/gfxfb, whose calls gfxfb_abi.h
 * defines. One display is behind the device at a time: gfxfb's own boot
 * display, which shows the framebuffer the boot firmware left, until a display
 * driver registers its display from its `attach()` and takes the boot
 * display's place for good. On a boot that left no framebuffer, the device is
 * published only when a driver registers its display, and shows that display
 * from the start. Userland calls gfxfb, and gfxfb calls the display.
 *
 * A display drives outputs, numbered from 0, each of which can show one
 * monitor's picture. An output is lit or dark, and a lit output has a base
 * buffer, the buffer it was lit with. The display owns that state, and gfxfb
 * asks for it. A lit output shows its base buffer until another buffer
 * presented on it takes its place. When gfxfb shows a display, it picks the
 * display's primary output: the lit boot output (the output the boot firmware
 * lit), else the lit output with the lowest index. Userland maps the primary
 * output's base buffer and draws into it, and `GFXFB_FLUSH` hands the changed
 * regions to the display's `flush()` for that output. While that output shows
 * a created buffer and holds no present of its base buffer, gfxfb skips the
 * display instead and passes the display no region with the base buffer's next
 * present, so a display that copies the buffer copies it whole. With no output
 * lit there is no primary output, so `GFXFB_GET_INFO` and `GFXFB_FLUSH` fail
 * and mmap at offset 0 is refused. A display shown when no memory was left to
 * record its lit outputs' base buffers has no primary output either.
 *
 * Userland also creates buffers, which the display allocates in
 * `create_buffer()`, and presents buffers on outputs, which gfxfb hands to the
 * display's `present()`. The display reports to `present_done()` how each
 * present it accepted ended, and gfxfb queues the end as an event for the open
 * file that presented. Userland gives an output's hardware cursor a picture
 * and moves it, which gfxfb hands to the display's `set_cursor()` and
 * `move_cursor()`. One open file at a time controls the buffers, from its
 * first creation, present or cursor call that succeeds until it closes. gfxfb
 * keeps a reference-counted record of each buffer, every lit output's base
 * buffer included, and hands the buffer back to the display's
 * `release_buffer()` once no handle, output or mapping refers to it. Once a
 * display call returns ERR_DEVICE or a present ends with
 * `GFXFB_PRESENT_FAILED`, gfxfb fails every later `GFXFB_FLUSH`,
 * `GFXFB_PRESENT`, `GFXFB_SET_CURSOR` and `GFXFB_MOVE_CURSOR` without handing
 * it to the display.
 */

namespace gfxfb {

constexpr int32_t OK              = 0;
constexpr int32_t ERR             = -1;
constexpr int32_t ERR_INVALID_ARG = -2;
constexpr int32_t ERR_DARK        = -3; // the output is not lit
constexpr int32_t ERR_DEVICE      = -4; // the display's hardware failed for good
constexpr int32_t ERR_NO_MEMORY   = -5;
constexpr int32_t ERR_UNSUPPORTED = -6; // the display creates or presents no buffers, or the output has no cursor
constexpr int32_t ERR_REFUSED     = -7; // the hardware refused the call and stays usable

// The widest and tallest cursor picture a display may report for an output, which bounds the pictures gfxfb copies
constexpr uint32_t CURSOR_DIMENSION_MAX = 256;

/**
 * One output's state as its display reports it. Its cursor maximum is the
 * largest picture its hardware cursor shows, at most CURSOR_DIMENSION_MAX in
 * each dimension, or 0 by 0 for an output without a hardware cursor.
 */
struct output_state {
    bool       lit;
    bool       boot_output;       // the boot firmware lit this output
    gfxfb_mode current;           // the mode it is lit at, meaningful while lit
    uint32_t   cursor_max_width;  // the widest cursor picture it shows
    uint32_t   cursor_max_height; // the tallest
};

/**
 * Pixel memory holding a picture: the boot framebuffer on the boot display,
 * memory the display's driver allocated on a driver display, as an output's
 * base buffer or from `create_buffer()`. A driver display's buffers hold
 * 32-bit pixels with red in bits 23 to 16, green in bits 15 to 8 and blue in
 * bits 7 to 0. gfxfb copies the fields when it takes a buffer, and a driver
 * may derive from it to keep its own state.
 */
struct buffer {
    uint32_t         width;
    uint32_t         height;
    uint32_t         pitch;     // bytes per row
    uint64_t         size;      // bytes userland maps, whole pages holding pitch * height bytes
    pmm::phys_addr_t phys;      // physically contiguous for `size` bytes
    bool             cacheable; // false maps it write-combining
};

/**
 * How a present the display accepted ended, as it reports to `present_done()`.
 * gfxfb reads `frame` and `timestamp_ns` only when the present latched. A
 * display whose hardware has no vertical blanks counts boundaries of its own
 * in their place, as `display::present()` says.
 */
struct present_result {
    uint32_t status;       // GFXFB_PRESENT_*
    uint64_t frame;        // the output's vertical blanks since it was lit, counted at the latch
    uint64_t timestamp_ns; // when the latching vertical blank began, on the kernel's monotonic clock
};

/**
 * A cursor picture as gfxfb hands it to `display::set_cursor()`: `width` by
 * `height` pixels, row after row with no padding, and its hotspot (the pixel
 * that sits at the cursor's position). Each pixel holds alpha in bits 31 to 24
 * and red, green and blue as a buffer's pixel does, none multiplied by alpha.
 * A width of 0 hides the cursor, with every other field zero.
 */
struct cursor_image {
    const uint32_t* pixels; // in gfxfb's memory, valid only during the call
    uint32_t        width;
    uint32_t        height;
    uint32_t        hot_x;
    uint32_t        hot_y;
};

/**
 * A display behind /dev/gfxfb: one a device driver owns, or gfxfb's own boot
 * display. gfxfb calls `output_count()`, `read_output()` and `base_buffer()`
 * from `init()` and `register_display()`, on the boot thread, which cannot
 * sleep, so they must not wait. It calls `output_count()`, `read_output()`,
 * `needs_flush()`, `flush()`, `create_buffer()`, `present()`, `set_cursor()`
 * and `move_cursor()` from the tasks that use /dev/gfxfb, several tasks at
 * once, and `flush()`, `create_buffer()`, `present()` and the two cursor calls
 * may block the task that calls them. gfxfb never calls `present()` for one
 * output from two tasks at once, nor while a present the display accepted
 * there has not ended, and never makes two cursor calls for one output at
 * once, though a cursor call may run during a present for the same output. It
 * calls `release_buffer()` from the reaper, the kernel task that runs deferred
 * cleanup, and `release_buffer()` must never wait, since the reaper runs no
 * other cleanup until it returns. A display returns ERR_DEVICE only once its
 * hardware has failed for good, and from then on from every call that needs
 * the hardware, so a refusal that leaves the hardware usable is ERR_REFUSED,
 * or ERR_NO_MEMORY when the hardware has no memory left for a buffer. It
 * likewise ends a present with `GFXFB_PRESENT_FAILED` only once its hardware
 * has failed for good.
 */
class display {
public:
    virtual ~display() = default;

    /**
     * @brief Report how many outputs the display drives.
     * @return The count, at most GFXFB_OUTPUTS_MAX, which must stay fixed for
     * the display's life.
     */
    virtual uint32_t output_count() const = 0;

    /**
     * @brief Report whether drawing in a base buffer reaches the screen only
     * through `flush()`, as on a display that copies it elsewhere.
     * @return true when userland must flush what it draws.
     */
    virtual bool needs_flush() const = 0;

    /**
     * @brief Fill `out` with an output's state.
     * @param output Index of the output.
     * @param out Filled with the output's state on OK.
     * @return OK, or ERR_INVALID_ARG for an index of `output_count()` or more.
     */
    virtual int32_t read_output(uint32_t output, output_state& out) = 0;

    /**
     * @brief Return an output's base buffer, the buffer the output was lit
     * with. Once registered, the display must keep it until gfxfb hands it
     * back through `release_buffer()` and must return the same buffer for as
     * long as the output stays lit, since gfxfb keeps the pointer and userland
     * maps the memory.
     * @param output Index of the output, below the count.
     * @return The buffer, or nullptr while the output is dark.
     */
    virtual buffer* base_buffer(uint32_t output) = 0;

    /**
     * @brief Copy regions of an output's base buffer to the screen, or copy
     * nothing on a display that scans the base buffer out directly.
     * @param output Index of the output, below the count.
     * @param rects Regions inside the base buffer, none of them empty.
     * @param count Number of regions, at least one.
     * @return OK, ERR_DARK when the output is not lit, ERR_REFUSED when the
     * hardware refused the copy, or ERR_DEVICE once the hardware has failed.
     */
    virtual int32_t flush(uint32_t output, const gfxfb_rect* rects, uint32_t count) = 0;

    /**
     * @brief Allocate a buffer of `width` by `height` pixels in `format`,
     * picking its pitch and size.
     * @param width Width in pixels, at least 1.
     * @param height Height in pixels, at least 1.
     * @param format The format userland asked for, which gfxfb passes on
     * unchecked.
     * @param out Set on OK to the buffer, which the display must keep until
     * gfxfb hands it back through `release_buffer()`.
     * @return OK, ERR_INVALID_ARG for a size past the display's limit or a
     * format it does not support, ERR_NO_MEMORY when no memory is left for it,
     * ERR_UNSUPPORTED on a display that creates no buffers, ERR_REFUSED when
     * its hardware refused the buffer, or ERR_DEVICE once its hardware has
     * failed.
     */
    virtual int32_t create_buffer(uint32_t width, uint32_t height, uint32_t format, buffer** out) = 0;

    /**
     * @brief Give `buf` to the hardware to replace what `output` shows from
     * the output's next vertical blank, without waiting for that blank. A
     * display whose hardware has no vertical blanks counts boundaries of its
     * own at the output's refresh rate in their place. A display that shows a
     * copy of the buffer copies only `regions`, or the whole buffer when gfxfb
     * passes none, and a display that scans the buffer out directly ignores
     * them. The call may wait only for the display's own lock and for the
     * hardware to take the buffer, never for a vertical blank or for its
     * driver's task, and returns within a time the display states. The display
     * must end each present the call accepts exactly once, possibly before the
     * call returns and at the latest GFXFB_PRESENT_TIMEOUT_NS after it
     * returns, not counting time spent waiting for the CPU, by passing its
     * result to `present_done()` from its interrupt handler, or elevated from
     * a task or a timer's callback. It may hold its own locks then, since
     * `present_done()` never sleeps or calls the display.
     * @param output Index of the output, below the count.
     * @param buf A buffer from `create_buffer()` or `base_buffer()`.
     * @param regions The regions of `buf` that userland named as changed since
     * `buf` was created or last presented, each wholly inside `buf` and none
     * of them empty, or null when `region_count` is 0, as for a base buffer
     * holding changes that gfxfb skipped flushing. gfxfb frees them once the
     * call returns, so a display reading them later must copy them.
     * @param region_count Number of regions, at most GFXFB_MAX_FLUSH_RECTS.
     * @return OK once the hardware was given the buffer. Otherwise the display
     * must not have given it the buffer, and returns the first that holds of
     * ERR_UNSUPPORTED on a display that presents no buffers, ERR_DEVICE once
     * the hardware has failed, ERR_DARK when the output is not lit,
     * ERR_INVALID_ARG when the buffer's width or height differs from the
     * output's mode, and ERR_REFUSED when the hardware refused the buffer.
     */
    virtual int32_t present(uint32_t output, buffer* buf, const gfxfb_rect* regions, uint32_t region_count) = 0;

    /**
     * @brief Give an output's hardware cursor `image`, or hide the cursor when
     * `image.width` is 0. The display must copy the picture before returning,
     * converting it if its hardware wants another form. The call may wait only
     * for the display's own lock, for the hardware to take the picture and for
     * the copy, never for a vertical blank or for its driver's task, and
     * returns within a time the display states.
     * @param output Index of the output, below the count.
     * @param image The picture with its hotspot inside it, or a hide with a
     * width of 0. gfxfb frees its pixels once the call returns.
     * @return OK once the hardware was given the picture, or hid the cursor.
     * Otherwise the display returns the first that holds of ERR_UNSUPPORTED on
     * an output without a hardware cursor, ERR_DEVICE once the hardware has
     * failed, ERR_DARK when the output is not lit, ERR_INVALID_ARG for a
     * picture wider or taller than the output's cursor maximum, and
     * ERR_REFUSED when the hardware refused the call.
     */
    virtual int32_t set_cursor(uint32_t output, const cursor_image& image) = 0;

    /**
     * @brief Place the hotspot of an output's cursor at (`x`, `y`) in the
     * output's pixels, even off the output. A move while the cursor is hidden,
     * before any picture or after a hide, only records the position, and the
     * cursor stays hidden. The call may wait only for the display's own lock
     * and for the hardware to take the position, within
     * GFXFB_PRESENT_TIMEOUT_NS of giving it, never for a vertical blank or for
     * its driver's task.
     * @param output Index of the output, below the count.
     * @param x Column of the hotspot, negative left of the output.
     * @param y Row of the hotspot, negative above the output.
     * @return OK once the hardware was given the position or, while the cursor
     * is hidden, once the display recorded it. Otherwise the first that holds
     * of ERR_UNSUPPORTED on an output without a hardware cursor, ERR_DEVICE
     * once the hardware has failed, ERR_DARK when the output is not lit, and
     * ERR_REFUSED when the hardware refused the move.
     */
    virtual int32_t move_cursor(uint32_t output, int32_t x, int32_t y) = 0;

    /**
     * @brief Take back a buffer from `create_buffer()` or `base_buffer()` once
     * nothing refers to it, freeing it unless the display keeps it for good,
     * as the boot display keeps the boot framebuffer. Runs once per buffer, in
     * the reaper's task.
     * @param buf The buffer.
     */
    virtual void release_buffer(buffer* buf) = 0;
};

/**
 * @brief Initialize the framebuffer device and register /dev/gfxfb.
 * No-op if no framebuffer is available from the bootloader.
 * @return OK on success or if no framebuffer, ERR on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Report whether a driver display is registered behind /dev/gfxfb, in
 * which case `register_display` refuses any other. A driver asks before it
 * prepares a display, so it can decline instead of preparing one that would
 * be refused.
 * @return true when a driver display is registered.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool has_driver_display();

/**
 * @brief Report whether the boot display is behind /dev/gfxfb, in which case
 * `register_display` accepts only a display with an output lit.
 * @return true while gfxfb shows the boot framebuffer.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool has_boot_display();

/**
 * @brief Put a driver display behind /dev/gfxfb. It must be called from a PCI
 * driver's `attach()`, before userland starts, with every output's state
 * ready, since gfxfb reads those states to pick the primary output. While the
 * boot display is behind /dev/gfxfb, a display with no output lit is refused,
 * so a card that lit nothing leaves userland the boot framebuffer. On a boot
 * that gave no framebuffer, a display is accepted with every output dark, and
 * with no primary output when no memory is left to record its lit outputs'
 * base buffers.
 * @param driver_display The display. It must never be freed.
 * @return OK, ERR for a null display, or ERR with a line logged when a driver
 * display is already registered, when a program has opened /dev/gfxfb, or when
 * gfxfb cannot create or publish its node. While the boot display is behind
 * /dev/gfxfb, it also returns ERR with a line logged for a display with no
 * output lit or when no memory is left to record the base buffers of its lit
 * outputs.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_display(display* driver_display);

/**
 * @brief End a present that a display accepted, by recording the end and
 * queuing it as an event for the open file that presented. The display must
 * call it exactly once for each present it accepted, as `display::present()`
 * describes. It never sleeps or calls the display, so a display may call it
 * from its interrupt handler or holding its own locks.
 * @param driver_display The display that accepted the present. gfxfb drops the
 * end, logging an error, when /dev/gfxfb does not show it.
 * @param output Index of the output the present was for. gfxfb drops the end,
 * logging an error, when the output holds no unended present, as once that
 * present has ended or for an index of the display's output count or more.
 * @param result How the present ended.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void present_done(display* driver_display, uint32_t output, const present_result& result);

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_H
