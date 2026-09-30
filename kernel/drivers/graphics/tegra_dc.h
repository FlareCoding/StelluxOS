#ifndef STELLUX_DRIVERS_GRAPHICS_TEGRA_DC_H
#define STELLUX_DRIVERS_GRAPHICS_TEGRA_DC_H

#include "common/types.h"

namespace tegra_dc {

constexpr int32_t OK = 0;
constexpr int32_t ERR_UNAVAILABLE = -1;
constexpr int32_t ERR_UNSUPPORTED = -2;
constexpr int32_t ERR_UNSAFE = -3;
constexpr int32_t ERR_MAP = -4;

/**
 * @brief Adopt the framebuffer the Tegra210 boot firmware left on screen.
 *
 * cboot lights up HDMI and scans out its boot logo before U-Boot runs, but
 * U-Boot has no video driver, so no EFI GOP reaches Limine. When the display
 * controller is still powered and scanning out a 32bpp window, its buffer
 * geometry is read back from the DC registers and published through
 * g_boot_info.framebuffer, like a simplefb handoff.
 *
 * Refuses (returns an error, leaves g_boot_info untouched) unless every page
 * of the buffer is outside the memory the PMM hands out.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t adopt_boot_framebuffer();

/**
 * @brief Find the boot logo framebuffer cboot published on the kernel
 * command line as "tegra_fbmem=<size>@<base>".
 *
 * U-Boot on the Nano leaves this range out of the EFI memory map (the
 * fb0_carveout it copies into the device tree has an empty reg), so it
 * arrives as usable RAM and must be reserved before the PMM hands it out.
 * Reads the bootloader DTB directly, so it works before fdt::init().
 * @return true and fills base/size when the parameter is present.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool boot_fbmem(uint64_t* out_base, uint64_t* out_size);

} // namespace tegra_dc

#endif // STELLUX_DRIVERS_GRAPHICS_TEGRA_DC_H
