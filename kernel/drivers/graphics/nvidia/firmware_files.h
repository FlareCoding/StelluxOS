// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

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
 * The header at the start of a bootloader or booter file, six little-endian
 * 32-bit fields, as NVIDIA's extraction tool writes it: the script in NVIDIA's
 * open kernel modules that writes those programs out as files. Between the
 * header and the program lie the descriptors, records that say where the
 * program's parts and signatures lie, and the signatures each kind of file
 * carries.
 */
struct boot_file_header {
    uint32_t magic;             // 0x10DE
    uint32_t header_version;    // 1
    uint32_t rounded_size;      // the file's length rounded up to a multiple of 256 bytes, so it can exceed that length
    uint32_t descriptor_offset; // where the descriptors start, right after this header
    uint32_t program_offset;    // where the program starts, at or after the descriptors
    uint32_t program_size;      // bytes
};

static_assert(sizeof(boot_file_header) == 24, "boot_file_header must be 24 bytes");

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

/**
 * @brief Check that a bootloader or booter starts with a `boot_file_header`
 * whose descriptors start right after it and whose program starts at or after
 * the descriptors and ends inside the file. The rounded size, which can exceed
 * the file's length, is not compared with it. The header is read with
 * `fw::file::read`, which takes the file's mutex, so this must not run in an
 * interrupt handler or under a spinlock. On the boot thread, which cannot
 * wait, `file` must be one no other thread uses at the time, as `fw::file`
 * describes.
 * @param file Must hold a file open.
 * @param name What the file is, such as "bootloader", for the log line.
 * @return `OK`, or `ERR_BAD_FIRMWARE` when the file is shorter than the
 * header, the magic is not 0x10DE or the header version not 1, the
 * descriptors do not start right after the header, the program starts before
 * them or ends past the file, or reading fails. The cause is logged, naming
 * `name`.
 */
int32_t check_boot_file(const fw::file& file, const char* name);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FIRMWARE_FILES_H
