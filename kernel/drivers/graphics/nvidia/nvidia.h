#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H

#include "common/types.h"

namespace drivers::nvidia {

// Result codes shared by every part of the driver
constexpr int32_t OK                 = 0;
constexpr int32_t ERR_NOT_RESPONDING = -1; // `PMC_BOOT_0` reads all ones, so the card does not respond
constexpr int32_t ERR_BAD_VBIOS      = -2; // the VBIOS lacks a structure bring-up reads from it, or one is malformed
                                           // or ends past the ROM window or the VBIOS
constexpr int32_t ERR_BAD_FIRMWARE   = -3; // a firmware file is missing, unreadable, malformed or of another version,
                                           // or its name invalid
constexpr int32_t ERR_TIMEOUT        = -4; // the card did not finish a step within its deadline
constexpr int32_t ERR_BAD_STATE      = -5; // the card's registers show a state bring-up cannot proceed from, such as
                                           // protected memory already set up, or FWSEC not fencing off the FRTS region
constexpr int32_t ERR_NO_MEMORY      = -6; // allocating memory for the card to reach failed

/**
 * @brief Whether the `bytes` bytes at `offset` lie inside the first `limit`
 * bytes, also when `offset + bytes` would overflow.
 */
inline bool lies_inside(uint64_t offset, uint64_t bytes, uint64_t limit) {
    return offset <= limit && bytes <= limit - offset;
}

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
