#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_H

#include "common/types.h"
#include "drivers/graphics/gfxfb_abi.h"
#include "mm/pmm_types.h"

namespace gfxfb {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

/**
 * The pixel memory behind /dev/gfxfb and the layout userland draws in.
 */
struct surface {
    pmm::phys_addr_t phys; // physically contiguous, pitch * height bytes
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    bool     cacheable;
};

/**
 * A display that a device driver owns. Userland draws into its surface
 * through /dev/gfxfb, then `GFXFB_FLUSH` hands the changed regions to
 * `flush()` which copies them to the screen.
 */
class display {
public:
    virtual ~display() = default;

    /**
     * @brief Copy regions of the surface to the screen. Runs in the task
     * that called `GFXFB_FLUSH` and may block it. Several tasks can call
     * it at once.
     * @param rects Regions inside the surface, none of them empty.
     * @param count Number of regions, at least one.
     * @return OK on success, ERR on failure.
     */
    virtual int32_t flush(const gfxfb_rect* rects, uint32_t count) = 0;
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
 * @brief Put a driver-owned display behind /dev/gfxfb in place of the boot
 * framebuffer. Only the first driver display is accepted. It must be
 * registered before userland starts, as from a driver's `attach()`.
 * @param fb Surface userland maps and draws into. Its memory must never be freed.
 * @param driver_display Display that flushes `fb`. It must never be freed.
 * @return OK on success, ERR if a driver display already exists or
 * registration fails.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_display(const surface& fb, display* driver_display);

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_H
