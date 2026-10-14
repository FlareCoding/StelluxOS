#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H

#include "common/types.h"

namespace drivers::nvidia {

// Result codes shared by every part of the driver
constexpr int32_t OK                 = 0;
constexpr int32_t ERR_NOT_RESPONDING = -1; // `PMC_BOOT_0` reads all ones, so the card does not respond
constexpr int32_t ERR_BAD_VBIOS      = -2; // the ROM window holds no chain of images `read_vbios_layout` accepts

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_H
