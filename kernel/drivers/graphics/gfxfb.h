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
 * monitor's picture. An output is lit or dark, and a lit output shows its base
 * buffer. The display owns that state, and gfxfb asks for it. When gfxfb shows
 * a display, it picks the display's primary output: the lit boot output (the
 * output the boot firmware lit), else the lit output with the lowest index.
 * Userland maps the primary output's base buffer and draws into it, and
 * `GFXFB_FLUSH` hands the changed regions to the display's `flush()` for that
 * output. With no output lit there is no primary output, so `GFXFB_GET_INFO`
 * and `GFXFB_FLUSH` fail and mmap is refused.
 */

namespace gfxfb {

constexpr int32_t OK              = 0;
constexpr int32_t ERR             = -1;
constexpr int32_t ERR_INVALID_ARG = -2;
constexpr int32_t ERR_DARK        = -3; // the output is not lit
constexpr int32_t ERR_DEVICE      = -4; // the display's hardware failed the call

/**
 * One output's state as its display reports it.
 */
struct output_state {
    bool lit;
    bool boot_output; // the boot firmware lit this output
};

/**
 * Pixel memory holding the picture an output shows: the boot framebuffer on
 * the boot display, memory the display's driver allocated on a driver display.
 * A driver display's buffers hold 32-bit pixels with red in bits 23 to 16,
 * green in bits 15 to 8 and blue in bits 7 to 0.
 */
struct buffer {
    uint32_t         width;
    uint32_t         height;
    uint32_t         pitch;     // bytes per row
    pmm::phys_addr_t phys;      // physically contiguous, pitch * height bytes
    bool             cacheable; // false maps it write-combining
};

/**
 * A display behind /dev/gfxfb: one a device driver owns, or gfxfb's own boot
 * display. gfxfb calls `output_count()`, `read_output()` and `base_buffer()`
 * only from `init()` and `register_display()`, on the boot thread, which
 * cannot sleep, so they must not wait. It calls `needs_flush()` and `flush()`
 * from the tasks that use /dev/gfxfb, several at once, and `flush()` may block
 * them.
 */
class display {
public:
    virtual ~display() = default;

    /**
     * @brief Report how many outputs the display drives.
     * @return The count, which must stay fixed for the display's life.
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
     * @brief Return an output's base buffer, the buffer it shows while lit.
     * Once registered, the display must never free it and must return the same
     * buffer for as long as the output stays lit, since gfxfb keeps the
     * pointer and userland maps the memory.
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
     * @return OK, ERR_DARK when the output is not lit, or ERR_DEVICE when the
     * display's hardware failed the copy.
     */
    virtual int32_t flush(uint32_t output, const gfxfb_rect* rects, uint32_t count) = 0;
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
 * ready, since gfxfb reads the states once, to pick the primary output. While
 * the boot display is behind /dev/gfxfb, a display with no output lit is
 * refused, so a card that lit nothing leaves userland the boot framebuffer. On
 * a boot that gave no framebuffer, a display is accepted with every output
 * dark.
 * @param driver_display The display. It must never be freed.
 * @return OK, ERR for a null display, or ERR with a line logged when a driver
 * display is already registered, when a program has opened /dev/gfxfb, for a
 * display with no output lit while the boot display is behind /dev/gfxfb, or
 * when gfxfb cannot create or publish its node.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_display(display* driver_display);

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_H
