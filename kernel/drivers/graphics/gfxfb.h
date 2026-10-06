#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_H

#include "common/types.h"
#include "mm/pmm_types.h"

namespace gfxfb {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

constexpr uint32_t GFXFB_GET_INFO = 0x4700;
constexpr uint32_t GFXFB_FLUSH    = 0x4701;

constexpr uint8_t GFXFB_INFO_NEEDS_FLUSH = 0x01; // drawing reaches the screen only through GFXFB_FLUSH

constexpr uint32_t GFXFB_MAX_FLUSH_RECTS = 256;

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
