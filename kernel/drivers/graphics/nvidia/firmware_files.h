#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_FILES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_FILES_H

#include "drivers/graphics/nvidia/chip.h"
#include "fw/fw.h"

namespace drivers::nvidia {

// The firmware version the driver boots on every chip, which names the directory under nvidia/ in
// `fw::DIRECTORY` that holds its files
constexpr const char FIRMWARE_VERSION[] = "535.183.01";

// The firmware files bring-up opens, each open until the object is destroyed when `attach()` returns
struct firmware_files {
    fw::file bootloader;
    fw::file booter_load;
    fw::file gsp_image;
};

/**
 * @brief Open the files a chip boots from, in the layout the nvidia-firmware
 * package installs under `fw::DIRECTORY`: the bootloader and booter load in
 * the chip's boot directory in nvidia/<FIRMWARE_VERSION>/ first, then the GSP
 * image in nvidia/<FIRMWARE_VERSION>/, logging each file's name and size.
 * `files_out` must hold no file open, and keeps every file that opened until
 * it is destroyed, whatever the result.
 * @return `OK`, `DECLINED` when a directory the files belong in is not
 * installed, or `ERR_BAD_FIRMWARE` when a file of an installed directory
 * cannot be opened, a missing file included, or when a name built from
 * `firmware` is too long or malformed. The cause is logged.
 */
int32_t open_firmware_files(const chip_firmware& firmware, firmware_files& files_out);

/**
 * @brief Translate a `fw::file` result into what it means for bring-up.
 * @return `OK` for `fw::OK`, `DECLINED` for `fw::ERR_NOT_INSTALLED`, and
 * `ERR_BAD_FIRMWARE` for every other result.
 */
int32_t to_bring_up_result(int32_t fw_result);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_FILES_H
