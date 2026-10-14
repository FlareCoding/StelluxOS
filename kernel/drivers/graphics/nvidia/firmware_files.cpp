// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/pci_driver.h"
#include "common/logging.h"
#include "common/string.h"

namespace drivers::nvidia {

// The directory in `fw::DIRECTORY` holding NVIDIA's firmware, one directory per firmware version
constexpr const char NVIDIA_DIRECTORY[] = "nvidia";

// File names of the bootloader and the booter that loads the GSP image, in a chip's boot directory
constexpr const char BOOTLOADER_FILE[]  = "bootloader.bin";
constexpr const char BOOTER_LOAD_FILE[] = "booter_load.bin";

// Room for every name built from a chip table entry, with its NUL. A longer name fails with `ERR_BAD_FIRMWARE`.
constexpr size_t NAME_CAPACITY = 128;

// The names `open_firmware_files` builds, relative to `fw::DIRECTORY`: three files and the directories they lie in
struct firmware_names {
    char version_directory[NAME_CAPACITY];
    char boot_directory[NAME_CAPACITY];
    char bootloader[NAME_CAPACITY];
    char booter_load[NAME_CAPACITY];
    char gsp_image[NAME_CAPACITY];
};

// The magic and header version a valid `boot_file_header` holds
constexpr uint32_t BOOT_FILE_MAGIC          = 0x10DE;
constexpr uint32_t BOOT_FILE_HEADER_VERSION = 1;

// Writes `directory`, a '/' and `file` into `name_out`, which must hold `NAME_CAPACITY` bytes. Returns false when
// they and a NUL do not fit.
static bool join_name(char* name_out, const char* directory, const char* file) {
    size_t directory_length = string::strlen(directory);
    size_t file_length = string::strlen(file);
    if (directory_length + 1 + file_length >= NAME_CAPACITY) {
        return false;
    }

    string::memcpy(name_out, directory, directory_length);
    name_out[directory_length] = '/';
    string::memcpy(name_out + directory_length + 1, file, file_length + 1);

    return true;
}

// Builds the names in the layout the nvidia-firmware package installs, the bootloader and booter load in
// nvidia/<version>/<boot directory>/ and the GSP image in nvidia/<version>/. Returns false when one does not fit.
static bool build_names(const chip_firmware& firmware, firmware_names& names_out) {
    return join_name(names_out.version_directory, NVIDIA_DIRECTORY, FIRMWARE_VERSION)
        && join_name(names_out.boot_directory, names_out.version_directory, firmware.boot_directory)
        && join_name(names_out.bootloader, names_out.boot_directory, BOOTLOADER_FILE)
        && join_name(names_out.booter_load, names_out.boot_directory, BOOTER_LOAD_FILE)
        && join_name(names_out.gsp_image, names_out.version_directory, firmware.gsp_image);
}

// Opens the file at `name`, which lies in `directory`, and logs the outcome
static int32_t open_firmware_file(const char* directory, const char* name, fw::file& file_out) {
    int32_t fw_result = file_out.open(name);
    int32_t rc = to_bring_up_result(fw_result);
    if (rc == DECLINED) {
        log::info(
            "nvidia: firmware directory %s is not installed, the nvidia-firmware package installs it",
            directory
        );
        return DECLINED;
    }

    if (fw_result == fw::ERR_MISSING) {
        log::error("nvidia: firmware file %s is missing from its installed directory", name);
        return rc;
    }

    if (rc != OK) {
        log::error("nvidia: opening firmware file %s failed with fw error %d", name, fw_result);
        return rc;
    }

    log::info("nvidia: firmware file %s, %lu bytes", name, file_out.size());

    return OK;
}

int32_t open_firmware_files(const chip_firmware& firmware, firmware_files& files_out) {
    firmware_names names;
    if (!build_names(firmware, names)) {
        log::error(
            "nvidia: a firmware name built from boot directory %s and GSP image %s is too long",
            firmware.boot_directory,
            firmware.gsp_image
        );
        return ERR_BAD_FIRMWARE;
    }

    int32_t rc = open_firmware_file(names.boot_directory, names.bootloader, files_out.bootloader);
    if (rc != OK) {
        return rc;
    }

    rc = open_firmware_file(names.boot_directory, names.booter_load, files_out.booter_load);
    if (rc != OK) {
        return rc;
    }

    return open_firmware_file(names.version_directory, names.gsp_image, files_out.gsp_image);
}

int32_t to_bring_up_result(int32_t fw_result) {
    if (fw_result == fw::OK) {
        return OK;
    }

    if (fw_result == fw::ERR_NOT_INSTALLED) {
        return DECLINED;
    }

    return ERR_BAD_FIRMWARE;
}

int32_t check_boot_file(const fw::file& file, const char* name) {
    boot_file_header header = {};
    if (file.size() < sizeof(header)) {
        log::error("nvidia: the %s is %lu bytes, shorter than its 24-byte header", name, file.size());
        return ERR_BAD_FIRMWARE;
    }

    int32_t fw_result = file.read(0, &header, sizeof(header));
    int32_t rc = to_bring_up_result(fw_result);
    if (rc != OK) {
        log::error("nvidia: reading the %s failed with fw error %d", name, fw_result);
        return rc;
    }

    bool has_magic = header.magic == BOOT_FILE_MAGIC;
    bool has_version = header.header_version == BOOT_FILE_HEADER_VERSION;
    if (!has_magic || !has_version) {
        log::error(
            "nvidia: the %s starts with magic 0x%X and header version %u instead of magic 0x10DE and version 1",
            name,
            header.magic,
            header.header_version
        );
        return ERR_BAD_FIRMWARE;
    }

    bool descriptors_follow_header = header.descriptor_offset == sizeof(header);
    bool program_follows_descriptors = header.program_offset >= header.descriptor_offset;
    if (!descriptors_follow_header || !program_follows_descriptors) {
        log::error(
            "nvidia: the %s's header places its descriptors at %u and its program at %u, where the descriptors must "
            "start right after the 24-byte header and the program at or after them",
            name,
            header.descriptor_offset,
            header.program_offset
        );
        return ERR_BAD_FIRMWARE;
    }

    uint64_t program_end = static_cast<uint64_t>(header.program_offset) + header.program_size;
    if (program_end > file.size()) {
        log::error(
            "nvidia: the %s's program of %u bytes at %u ends past the file's %lu bytes",
            name,
            header.program_size,
            header.program_offset,
            file.size()
        );
        return ERR_BAD_FIRMWARE;
    }

    return OK;
}

} // namespace drivers::nvidia
