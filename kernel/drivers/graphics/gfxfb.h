#ifndef STELLUX_DRIVERS_GRAPHICS_GFXFB_H
#define STELLUX_DRIVERS_GRAPHICS_GFXFB_H

#include "common/types.h"

namespace gfxfb {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

constexpr uint32_t GFXFB_GET_INFO = 0x4700;

struct gfxfb_info {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t  padding[3];
    uint64_t size;
};

/**
 * @brief Initialize the framebuffer device and register /dev/gfxfb.
 * No-op if no framebuffer is available from the bootloader.
 * @return OK on success or if no framebuffer, ERR on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace gfxfb

#endif // STELLUX_DRIVERS_GRAPHICS_GFXFB_H
