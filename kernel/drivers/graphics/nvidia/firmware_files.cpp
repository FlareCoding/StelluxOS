// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/pci_driver.h"
#include "common/logging.h"
#include "common/string.h"
#include "exec/elf64.h"

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

// Where each field the driver reads lies in the bootloader's descriptor, in bytes from its start, and the bytes up to
// the last one's end. Each is a little-endian 32-bit field. The driver reads offsets and sizes as bytes of the program.
constexpr uint32_t BOOTLOADER_APP_VERSION_FIELD         = 0x1C;
constexpr uint32_t BOOTLOADER_MANIFEST_OFFSET_FIELD     = 0x20;
constexpr uint32_t BOOTLOADER_MANIFEST_SIZE_FIELD       = 0x24;
constexpr uint32_t BOOTLOADER_MONITOR_DATA_OFFSET_FIELD = 0x28;
constexpr uint32_t BOOTLOADER_MONITOR_DATA_SIZE_FIELD   = 0x2C;
constexpr uint32_t BOOTLOADER_MONITOR_CODE_OFFSET_FIELD = 0x30;
constexpr uint32_t BOOTLOADER_MONITOR_CODE_SIZE_FIELD   = 0x34;
constexpr uint32_t BOOTLOADER_DESCRIPTOR_FIELD_BYTES    = 0x38;

// The GSP image's section that holds its firmware version, a string and its NUL
constexpr const char VERSION_SECTION[] = ".fwversion";

// A bound on the section headers and names the check reads, several times the section count of each GSP image the
// chip table names. An image with more sections fails with `ERR_BAD_FIRMWARE`.
constexpr uint16_t GSP_IMAGE_MAX_SECTIONS = 64;

// Room for a version string of up to 31 bytes and its NUL, so a .fwversion section holding another version can be
// logged. A larger section fails unread.
constexpr size_t VERSION_CAPACITY = 32;

static_assert(sizeof(FIRMWARE_VERSION) <= VERSION_CAPACITY, "FIRMWARE_VERSION must fit the version buffer");

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

// Reads `count` bytes at `offset` of `file` into `dst`, logging a failure with `name` saying what the file is
static int32_t read_firmware(const fw::file& file, const char* name, uint64_t offset, void* dst, size_t count) {
    int32_t fw_result = file.read(offset, dst, count);
    int32_t rc = to_bring_up_result(fw_result);
    if (rc != OK) {
        log::error("nvidia: reading the %s failed with fw error %d", name, fw_result);
    }

    return rc;
}

static int32_t read_gsp_image(const fw::file& image, uint64_t offset, void* dst, size_t count) {
    return read_firmware(image, "GSP image", offset, dst, count);
}

// Callers check first that the section table lies inside the file and that `index` is below its section count, so
// the sum cannot wrap
static uint64_t section_header_offset(const elf64::Ehdr& header, uint16_t index) {
    return header.e_shoff + static_cast<uint64_t>(index) * sizeof(elf64::Shdr);
}

// Reads the GSP image's ELF header and checks that the file is a 64-bit little-endian ELF file with at most
// `GSP_IMAGE_MAX_SECTIONS` section headers of 64 bytes inside it, and a name table index from 1 to the last section
static int32_t read_elf_header(const fw::file& image, elf64::Ehdr& header_out) {
    if (image.size() < sizeof(header_out)) {
        log::error("nvidia: the GSP image is %lu bytes, shorter than its 64-byte ELF header", image.size());
        return ERR_BAD_FIRMWARE;
    }

    int32_t rc = read_gsp_image(image, 0, &header_out, sizeof(header_out));
    if (rc != OK) {
        return rc;
    }

    const uint8_t* ident = header_out.e_ident;
    bool has_magic = ident[0] == elf64::ELFMAG0
        && ident[1] == elf64::ELFMAG1
        && ident[2] == elf64::ELFMAG2
        && ident[3] == elf64::ELFMAG3;
    bool is_64_bit = ident[elf64::EI_CLASS] == elf64::ELFCLASS64;
    bool is_little_endian = ident[elf64::EI_DATA] == elf64::ELFDATA2LSB;
    if (!has_magic || !is_64_bit || !is_little_endian) {
        log::error("nvidia: the GSP image is not a 64-bit little-endian ELF file");
        return ERR_BAD_FIRMWARE;
    }

    if (header_out.e_shentsize != sizeof(elf64::Shdr)) {
        log::error("nvidia: the GSP image's section headers are %u bytes instead of 64", header_out.e_shentsize);
        return ERR_BAD_FIRMWARE;
    }

    if (header_out.e_shnum > GSP_IMAGE_MAX_SECTIONS) {
        log::error(
            "nvidia: the GSP image has %u sections, more than the %u the driver reads",
            header_out.e_shnum,
            GSP_IMAGE_MAX_SECTIONS
        );
        return ERR_BAD_FIRMWARE;
    }

    uint64_t table_bytes = static_cast<uint64_t>(header_out.e_shnum) * sizeof(elf64::Shdr);
    if (!lies_inside(header_out.e_shoff, table_bytes, image.size())) {
        log::error(
            "nvidia: the GSP image's section table of %lu bytes at offset %lu runs past the image's %lu bytes",
            table_bytes,
            header_out.e_shoff,
            image.size()
        );
        return ERR_BAD_FIRMWARE;
    }

    uint16_t name_table_index = header_out.e_shstrndx;
    bool is_null_section = name_table_index == elf64::SHN_UNDEF;
    bool is_past_last_section = name_table_index >= header_out.e_shnum;
    if (is_null_section || is_past_last_section) {
        log::error(
            "nvidia: the GSP image's section name table index %u must be at least 1 and below its section count %u",
            name_table_index,
            header_out.e_shnum
        );
        return ERR_BAD_FIRMWARE;
    }

    return OK;
}

// Reads the section header of the GSP image's name table and checks that the table lies inside the file
static int32_t read_name_table_header(const fw::file& image, const elf64::Ehdr& header, elf64::Shdr& name_table_out) {
    uint64_t name_table_header_offset = section_header_offset(header, header.e_shstrndx);
    int32_t rc = read_gsp_image(image, name_table_header_offset, &name_table_out, sizeof(name_table_out));
    if (rc != OK) {
        return rc;
    }

    if (!lies_inside(name_table_out.sh_offset, name_table_out.sh_size, image.size())) {
        log::error(
            "nvidia: the GSP image's section name table of %lu bytes at offset %lu runs past the image's %lu bytes",
            name_table_out.sh_size,
            name_table_out.sh_offset,
            image.size()
        );
        return ERR_BAD_FIRMWARE;
    }

    return OK;
}

// Finds the section named `VERSION_SECTION`, failing unless exactly one has that name. A name counts only when it
// and its NUL lie inside the name table.
static int32_t find_version_section(
    const fw::file& image,
    const elf64::Ehdr& header,
    const elf64::Shdr& name_table,
    elf64::Shdr& version_section_out
) {
    size_t version_section_count = 0;
    for (uint16_t i = 0; i < header.e_shnum; i++) {
        elf64::Shdr section = {};
        int32_t rc = read_gsp_image(image, section_header_offset(header, i), &section, sizeof(section));
        if (rc != OK) {
            return rc;
        }

        char name[sizeof(VERSION_SECTION)] = {};
        if (!lies_inside(section.sh_name, sizeof(name), name_table.sh_size)) {
            continue;
        }

        rc = read_gsp_image(image, name_table.sh_offset + section.sh_name, name, sizeof(name));
        if (rc != OK) {
            return rc;
        }

        if (string::memcmp(name, VERSION_SECTION, sizeof(name)) == 0) {
            version_section_out = section;
            version_section_count++;
        }
    }

    if (version_section_count != 1) {
        log::error("nvidia: the GSP image has %lu .fwversion sections instead of one", version_section_count);
        return ERR_BAD_FIRMWARE;
    }

    return OK;
}

// Checks that the .fwversion section lies inside the GSP image and holds exactly `FIRMWARE_VERSION` and its NUL
static int32_t check_version(const fw::file& image, const elf64::Shdr& version_section) {
    if (!lies_inside(version_section.sh_offset, version_section.sh_size, image.size())) {
        log::error(
            "nvidia: the GSP image's .fwversion section of %lu bytes at offset %lu runs past the image's %lu bytes",
            version_section.sh_size,
            version_section.sh_offset,
            image.size()
        );
        return ERR_BAD_FIRMWARE;
    }

    char version[VERSION_CAPACITY] = {};
    if (version_section.sh_size > sizeof(version)) {
        log::error(
            "nvidia: the GSP image's .fwversion section is %lu bytes, more than the driver's %lu-byte version buffer",
            version_section.sh_size,
            sizeof(version)
        );
        return ERR_BAD_FIRMWARE;
    }

    int32_t rc = read_gsp_image(image, version_section.sh_offset, version, version_section.sh_size);
    if (rc != OK) {
        return rc;
    }

    bool is_one_string = string::strnlen(version, version_section.sh_size) + 1 == version_section.sh_size;
    if (!is_one_string) {
        log::error(
            "nvidia: the GSP image's .fwversion section of %lu bytes does not hold exactly one string and its NUL",
            version_section.sh_size
        );
        return ERR_BAD_FIRMWARE;
    }

    if (string::strcmp(version, FIRMWARE_VERSION) != 0) {
        log::error("nvidia: the GSP image is firmware %s, but the driver boots only %s", version, FIRMWARE_VERSION);
        return ERR_BAD_FIRMWARE;
    }

    return OK;
}

int32_t check_gsp_image(const fw::file& image) {
    elf64::Ehdr header = {};
    int32_t rc = read_elf_header(image, header);
    if (rc != OK) {
        return rc;
    }

    elf64::Shdr name_table = {};
    rc = read_name_table_header(image, header, name_table);
    if (rc != OK) {
        return rc;
    }

    elf64::Shdr version_section = {};
    rc = find_version_section(image, header, name_table, version_section);
    if (rc != OK) {
        return rc;
    }

    return check_version(image, version_section);
}

int32_t check_boot_file(const fw::file& file, const char* name, boot_file_header& header_out) {
    boot_file_header header = {};
    if (file.size() < sizeof(header)) {
        log::error("nvidia: the %s is %lu bytes, shorter than its 24-byte header", name, file.size());
        return ERR_BAD_FIRMWARE;
    }

    int32_t rc = read_firmware(file, name, 0, &header, sizeof(header));
    if (rc != OK) {
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

    header_out = header;

    return OK;
}

// Checks that the part of the bootloader's program whose offset and size the descriptor holds at `offset_field` and
// `size_field` lies inside the program, naming the part `part` in the log line when it does not
static int32_t check_program_part(
    const uint8_t* descriptor,
    uint32_t offset_field,
    uint32_t size_field,
    uint32_t program_size,
    const char* part
) {
    uint32_t offset = read_u32(descriptor + offset_field);
    uint32_t size = read_u32(descriptor + size_field);
    if (!lies_inside(offset, size, program_size)) {
        log::error(
            "nvidia: the bootloader's %s of %u bytes at 0x%X ends past its program's %u bytes",
            part,
            size,
            offset,
            program_size
        );
        return ERR_BAD_FIRMWARE;
    }

    return OK;
}

int32_t read_bootloader_layout(
    const fw::file& bootloader,
    const boot_file_header& header,
    bootloader_layout& layout_out
) {
    uint32_t descriptor_bytes = header.program_offset - header.descriptor_offset;
    if (descriptor_bytes < BOOTLOADER_DESCRIPTOR_FIELD_BYTES) {
        log::error(
            "nvidia: the bootloader's descriptor is %u bytes, shorter than the %u bytes up to the end of the last "
            "field the driver reads",
            descriptor_bytes,
            BOOTLOADER_DESCRIPTOR_FIELD_BYTES
        );
        return ERR_BAD_FIRMWARE;
    }

    uint8_t descriptor[BOOTLOADER_DESCRIPTOR_FIELD_BYTES] = {};
    int32_t rc = read_firmware(bootloader, "bootloader", header.descriptor_offset, descriptor, sizeof(descriptor));
    if (rc != OK) {
        return rc;
    }

    rc = check_program_part(
        descriptor,
        BOOTLOADER_MANIFEST_OFFSET_FIELD,
        BOOTLOADER_MANIFEST_SIZE_FIELD,
        header.program_size,
        "manifest"
    );
    if (rc != OK) {
        return rc;
    }

    rc = check_program_part(
        descriptor,
        BOOTLOADER_MONITOR_DATA_OFFSET_FIELD,
        BOOTLOADER_MONITOR_DATA_SIZE_FIELD,
        header.program_size,
        "monitor data"
    );
    if (rc != OK) {
        return rc;
    }

    rc = check_program_part(
        descriptor,
        BOOTLOADER_MONITOR_CODE_OFFSET_FIELD,
        BOOTLOADER_MONITOR_CODE_SIZE_FIELD,
        header.program_size,
        "monitor code"
    );
    if (rc != OK) {
        return rc;
    }

    layout_out.manifest_offset = read_u32(descriptor + BOOTLOADER_MANIFEST_OFFSET_FIELD);
    layout_out.monitor_data_offset = read_u32(descriptor + BOOTLOADER_MONITOR_DATA_OFFSET_FIELD);
    layout_out.monitor_code_offset = read_u32(descriptor + BOOTLOADER_MONITOR_CODE_OFFSET_FIELD);
    layout_out.app_version = read_u32(descriptor + BOOTLOADER_APP_VERSION_FIELD);

    return OK;
}

} // namespace drivers::nvidia
