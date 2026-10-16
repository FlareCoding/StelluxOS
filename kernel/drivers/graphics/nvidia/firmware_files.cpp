// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/falcon.h"
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

// Where each field of booter load's descriptor header lies, in bytes from its start, and the header's size. Each is a
// little-endian 32-bit size or offset from the start of the file, the latter leading to a descriptor.
constexpr uint32_t BOOTER_SIGNATURES_OFFSET_FIELD      = 0x00;
constexpr uint32_t BOOTER_SIGNATURES_SIZE_FIELD        = 0x04;
constexpr uint32_t BOOTER_SLOT_DESCRIPTOR_FIELD        = 0x08; // the signature slot's offset from the program's start
constexpr uint32_t BOOTER_PATCH_INDEX_DESCRIPTOR_FIELD = 0x0C; // NVIDIA's signature patch index, which must be 0
constexpr uint32_t BOOTER_METADATA_OFFSET_FIELD        = 0x10;
constexpr uint32_t BOOTER_METADATA_SIZE_FIELD          = 0x14;
constexpr uint32_t BOOTER_COUNT_DESCRIPTOR_FIELD       = 0x18; // the signature count
constexpr uint32_t BOOTER_LOAD_HEADER_OFFSET_FIELD     = 0x1C;
constexpr uint32_t BOOTER_LOAD_HEADER_SIZE_FIELD       = 0x20;
constexpr uint32_t BOOTER_DESCRIPTOR_HEADER_BYTES      = 0x24;

// The metadata fields the driver reads, in bytes from its start, and the metadata's size
constexpr uint32_t BOOTER_ENGINE_MASK_FIELD = 0x04;
constexpr uint32_t BOOTER_UCODE_ID_FIELD    = 0x08;
constexpr uint32_t BOOTER_METADATA_BYTES    = 12;

// The load header fields the driver reads, in bytes from its start, and the load header's size. The code is the one
// app's, and the data is the one NVIDIA's driver loads with it.
constexpr uint32_t BOOTER_DATA_OFFSET_FIELD = 0x08;
constexpr uint32_t BOOTER_DATA_SIZE_FIELD   = 0x0C;
constexpr uint32_t BOOTER_APP_COUNT_FIELD   = 0x10;
constexpr uint32_t BOOTER_CODE_OFFSET_FIELD = 0x14;
constexpr uint32_t BOOTER_CODE_SIZE_FIELD   = 0x18;
constexpr uint32_t BOOTER_LOAD_HEADER_BYTES = 36;

// The GSP image's section that holds its firmware version, a string and its NUL
constexpr const char VERSION_SECTION[] = ".fwversion";

// The GSP image's section that holds the firmware the GSP runs
constexpr const char FIRMWARE_SECTION[] = ".fwimage";

// A bound on the section headers and names the check reads, several times the section count of each GSP image the
// chip table names. An image with more sections fails with `ERR_BAD_FIRMWARE`.
constexpr uint16_t GSP_IMAGE_MAX_SECTIONS = 64;

// Room for each section name the check looks for and its NUL, as many bytes as NVIDIA's driver gives the signature
// section's name. A longer name fails with `ERR_BAD_FIRMWARE`.
constexpr size_t SECTION_NAME_CAPACITY = 32;

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

// Finds the section named `name`, failing unless exactly one is, and checks that it holds at least one byte inside
// the file. A section's name counts only when the bytes `name` and its NUL take lie inside the name table.
static int32_t find_section(
    const fw::file& image,
    const elf64::Ehdr& header,
    const elf64::Shdr& name_table,
    const char* name,
    elf64::Shdr& section_out
) {
    size_t name_length = string::strnlen(name, SECTION_NAME_CAPACITY);
    if (name_length == SECTION_NAME_CAPACITY) {
        log::error(
            "nvidia: section name %s is longer than the %lu bytes of a GSP image section name the driver compares",
            name,
            SECTION_NAME_CAPACITY - 1
        );
        return ERR_BAD_FIRMWARE;
    }

    size_t name_bytes = name_length + 1;
    size_t match_count = 0;
    elf64::Shdr match = {};
    for (uint16_t i = 0; i < header.e_shnum; i++) {
        elf64::Shdr section = {};
        int32_t rc = read_gsp_image(image, section_header_offset(header, i), &section, sizeof(section));
        if (rc != OK) {
            return rc;
        }

        if (!lies_inside(section.sh_name, name_bytes, name_table.sh_size)) {
            continue;
        }

        char section_name[SECTION_NAME_CAPACITY] = {};
        rc = read_gsp_image(image, name_table.sh_offset + section.sh_name, section_name, name_bytes);
        if (rc != OK) {
            return rc;
        }

        if (string::memcmp(section_name, name, name_bytes) == 0) {
            match = section;
            match_count++;
        }
    }

    if (match_count != 1) {
        log::error("nvidia: the GSP image has %lu %s sections instead of one", match_count, name);
        return ERR_BAD_FIRMWARE;
    }

    if (match.sh_size == 0) {
        log::error("nvidia: the GSP image's %s section is empty", name);
        return ERR_BAD_FIRMWARE;
    }

    if (!lies_inside(match.sh_offset, match.sh_size, image.size())) {
        log::error(
            "nvidia: the GSP image's %s section of %lu bytes at offset %lu runs past the image's %lu bytes",
            name,
            match.sh_size,
            match.sh_offset,
            image.size()
        );
        return ERR_BAD_FIRMWARE;
    }

    section_out = match;

    return OK;
}

// Checks that the .fwversion section, which `find_section` found inside the GSP image, holds exactly
// `FIRMWARE_VERSION` and its NUL
static int32_t check_version(const fw::file& image, const elf64::Shdr& version_section) {
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

int32_t check_gsp_image(const fw::file& image, const char* signature_section, gsp_image_layout& layout_out) {
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
    rc = find_section(image, header, name_table, VERSION_SECTION, version_section);
    if (rc != OK) {
        return rc;
    }

    rc = check_version(image, version_section);
    if (rc != OK) {
        return rc;
    }

    elf64::Shdr firmware = {};
    rc = find_section(image, header, name_table, FIRMWARE_SECTION, firmware);
    if (rc != OK) {
        return rc;
    }

    elf64::Shdr signature = {};
    rc = find_section(image, header, name_table, signature_section, signature);
    if (rc != OK) {
        return rc;
    }

    layout_out.firmware_file_offset = firmware.sh_offset;
    layout_out.firmware_size = firmware.sh_size;
    layout_out.signature_file_offset = signature.sh_offset;
    layout_out.signature_size = signature.sh_size;

    return OK;
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

// Checks that the part of a boot file's program whose offset and size the descriptor holds at `offset_field` and
// `size_field` lies inside the program, naming the file `name` and the part `part` in the log line when it does not
static int32_t check_program_part(
    const uint8_t* descriptor,
    uint32_t offset_field,
    uint32_t size_field,
    uint32_t program_size,
    const char* name,
    const char* part
) {
    uint32_t offset = read_u32(descriptor + offset_field);
    uint32_t size = read_u32(descriptor + size_field);
    if (!lies_inside(offset, size, program_size)) {
        log::error(
            "nvidia: the %s's %s of %u bytes at 0x%X ends past its program's %u bytes",
            name,
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
        "bootloader",
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
        "bootloader",
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
        "bootloader",
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

// Reads the `count` bytes at `offset` of booter load into `dst` once they lie inside the file, naming them `name` in
// the log line when they end past it or the read fails
static int32_t read_booter_bytes(
    const fw::file& booter_load,
    const char* name,
    uint32_t offset,
    void* dst,
    uint32_t count
) {
    if (!lies_inside(offset, count, booter_load.size())) {
        log::error(
            "nvidia: the %s of %u bytes at 0x%X ends past the file's %lu bytes",
            name,
            count,
            offset,
            booter_load.size()
        );
        return ERR_BAD_FIRMWARE;
    }

    return read_firmware(booter_load, name, offset, dst, count);
}

// Reads the 32-bit descriptor of booter load that the descriptor header's field at `field` leads to, naming the
// descriptor `descriptor` in the log line when it ends past the file or the read fails
static int32_t read_booter_descriptor(
    const fw::file& booter_load,
    const uint8_t* descriptor_header,
    uint32_t field,
    const char* descriptor,
    uint32_t& value_out
) {
    uint8_t bytes[sizeof(uint32_t)] = {};
    uint32_t offset = read_u32(descriptor_header + field);
    int32_t rc = read_booter_bytes(booter_load, descriptor, offset, bytes, sizeof(bytes));
    if (rc != OK) {
        return rc;
    }

    value_out = read_u32(bytes);

    return OK;
}

// Reads the signature patch index and the signature count, checks them and the signatures' size as NVIDIA's driver
// does, then checks that the signatures end inside the file and records them in `layout`
static int32_t read_booter_signatures(
    const fw::file& booter_load,
    const uint8_t* descriptor_header,
    booter_layout& layout
) {
    uint32_t patch_index = 0;
    int32_t rc = read_booter_descriptor(
        booter_load,
        descriptor_header,
        BOOTER_PATCH_INDEX_DESCRIPTOR_FIELD,
        "booter load's signature patch index",
        patch_index
    );
    if (rc != OK) {
        return rc;
    }

    if (patch_index != 0) {
        log::error("nvidia: the booter load's signature patch index is %u, not 0", patch_index);
        return ERR_BAD_FIRMWARE;
    }

    uint32_t count = 0;
    rc = read_booter_descriptor(
        booter_load,
        descriptor_header,
        BOOTER_COUNT_DESCRIPTOR_FIELD,
        "booter load's signature count",
        count
    );
    if (rc != OK) {
        return rc;
    }

    uint32_t signatures_offset = read_u32(descriptor_header + BOOTER_SIGNATURES_OFFSET_FIELD);
    uint32_t signatures_size = read_u32(descriptor_header + BOOTER_SIGNATURES_SIZE_FIELD);
    bool has_signatures = count != 0 && signatures_size != 0;
    bool splits_evenly = has_signatures && signatures_size % count == 0;
    if (!splits_evenly) {
        log::error(
            "nvidia: the booter load's %u bytes of signatures do not split into %u signatures of one nonzero size",
            signatures_size,
            count
        );
        return ERR_BAD_FIRMWARE;
    }

    if (!lies_inside(signatures_offset, signatures_size, booter_load.size())) {
        log::error(
            "nvidia: the booter load's %u bytes of signatures at 0x%X end past the file's %lu bytes",
            signatures_size,
            signatures_offset,
            booter_load.size()
        );
        return ERR_BAD_FIRMWARE;
    }

    layout.signatures_offset = signatures_offset;
    layout.signature_size = signatures_size / count;
    layout.signature_count = count;

    return OK;
}

// Reads the metadata and the load header, checks that the load header lists one app and places the code and data
// inside the program, and records them in `layout`
static int32_t read_booter_parts(
    const fw::file& booter_load,
    const boot_file_header& header,
    const uint8_t* descriptor_header,
    booter_layout& layout
) {
    uint8_t metadata[BOOTER_METADATA_BYTES] = {};
    uint32_t metadata_offset = read_u32(descriptor_header + BOOTER_METADATA_OFFSET_FIELD);
    int32_t rc = read_booter_bytes(booter_load, "booter load's metadata", metadata_offset, metadata, sizeof(metadata));
    if (rc != OK) {
        return rc;
    }

    uint8_t load_header[BOOTER_LOAD_HEADER_BYTES] = {};
    uint32_t load_header_offset = read_u32(descriptor_header + BOOTER_LOAD_HEADER_OFFSET_FIELD);
    rc = read_booter_bytes(
        booter_load,
        "booter load's load header",
        load_header_offset,
        load_header,
        sizeof(load_header)
    );
    if (rc != OK) {
        return rc;
    }

    uint32_t app_count = read_u32(load_header + BOOTER_APP_COUNT_FIELD);
    if (app_count != 1) {
        log::error("nvidia: the booter load's load header lists %u apps, not 1", app_count);
        return ERR_BAD_FIRMWARE;
    }

    rc = check_program_part(
        load_header,
        BOOTER_CODE_OFFSET_FIELD,
        BOOTER_CODE_SIZE_FIELD,
        header.program_size,
        "booter load",
        "code"
    );
    if (rc != OK) {
        return rc;
    }

    rc = check_program_part(
        load_header,
        BOOTER_DATA_OFFSET_FIELD,
        BOOTER_DATA_SIZE_FIELD,
        header.program_size,
        "booter load",
        "data"
    );
    if (rc != OK) {
        return rc;
    }

    layout.code_offset = read_u32(load_header + BOOTER_CODE_OFFSET_FIELD);
    layout.code_size = read_u32(load_header + BOOTER_CODE_SIZE_FIELD);
    layout.data_offset = read_u32(load_header + BOOTER_DATA_OFFSET_FIELD);
    layout.data_size = read_u32(load_header + BOOTER_DATA_SIZE_FIELD);
    layout.ucode_id = read_u32(metadata + BOOTER_UCODE_ID_FIELD);
    layout.engine_mask = read_u32(metadata + BOOTER_ENGINE_MASK_FIELD);

    return OK;
}

// Reads the signature slot's offset, checks that the slot lies inside the data and ends before the program's end, and
// records its offset from the data's start in `layout`
static int32_t read_booter_slot(
    const fw::file& booter_load,
    const boot_file_header& header,
    const uint8_t* descriptor_header,
    booter_layout& layout
) {
    uint32_t slot = 0;
    int32_t rc = read_booter_descriptor(
        booter_load,
        descriptor_header,
        BOOTER_SLOT_DESCRIPTOR_FIELD,
        "booter load's signature slot offset",
        slot
    );
    if (rc != OK) {
        return rc;
    }

    uint32_t data_end = layout.data_offset + layout.data_size;
    bool starts_in_data = slot >= layout.data_offset;
    bool lies_in_data = starts_in_data && lies_inside(slot, layout.signature_size, data_end);
    if (!lies_in_data) {
        log::error(
            "nvidia: the booter load's %u-byte signature slot at byte 0x%X of its program is not inside its %u bytes "
            "of data at 0x%X",
            layout.signature_size,
            slot,
            layout.data_size,
            layout.data_offset
        );
        return ERR_BAD_FIRMWARE;
    }

    // NVIDIA's driver also refuses a slot that ends exactly at the program's end
    if (header.program_size - slot <= layout.signature_size) {
        log::error(
            "nvidia: the booter load's %u-byte signature slot at byte 0x%X of its program does not end before the "
            "program's %u bytes do",
            layout.signature_size,
            slot,
            header.program_size
        );
        return ERR_BAD_FIRMWARE;
    }

    layout.signature_slot = slot - layout.data_offset;

    return OK;
}

int32_t read_booter_layout(
    const fw::file& booter_load,
    const boot_file_header& header,
    booter_layout& layout_out
) {
    uint32_t descriptor_bytes = header.program_offset - header.descriptor_offset;
    if (descriptor_bytes < BOOTER_DESCRIPTOR_HEADER_BYTES) {
        log::error(
            "nvidia: the booter load's descriptors are %u bytes, shorter than their %u-byte header",
            descriptor_bytes,
            BOOTER_DESCRIPTOR_HEADER_BYTES
        );
        return ERR_BAD_FIRMWARE;
    }

    uint8_t descriptor_header[BOOTER_DESCRIPTOR_HEADER_BYTES] = {};
    int32_t rc = read_firmware(
        booter_load,
        "booter load",
        header.descriptor_offset,
        descriptor_header,
        sizeof(descriptor_header)
    );
    if (rc != OK) {
        return rc;
    }

    uint32_t metadata_size = read_u32(descriptor_header + BOOTER_METADATA_SIZE_FIELD);
    uint32_t load_header_size = read_u32(descriptor_header + BOOTER_LOAD_HEADER_SIZE_FIELD);
    bool has_metadata_size = metadata_size == BOOTER_METADATA_BYTES;
    bool has_load_header_size = load_header_size == BOOTER_LOAD_HEADER_BYTES;
    if (!has_metadata_size || !has_load_header_size) {
        log::error(
            "nvidia: the booter load's metadata is %u bytes and its load header %u, where they must be %u and %u",
            metadata_size,
            load_header_size,
            BOOTER_METADATA_BYTES,
            BOOTER_LOAD_HEADER_BYTES
        );
        return ERR_BAD_FIRMWARE;
    }

    booter_layout layout = {};
    rc = read_booter_signatures(booter_load, descriptor_header, layout);
    if (rc != OK) {
        return rc;
    }

    rc = read_booter_parts(booter_load, header, descriptor_header, layout);
    if (rc != OK) {
        return rc;
    }

    rc = read_booter_slot(booter_load, header, descriptor_header, layout);
    if (rc != OK) {
        return rc;
    }

    layout_out = layout;

    return OK;
}

int32_t select_booter_signature(
    const registers& regs,
    const booter_layout& booter_load,
    booter_signature& signature_out
) {
    uint32_t fuse_version = read_ucode_fuse_version(regs, falcon_id::sec2, booter_load.ucode_id);
    uint32_t last_index = booter_load.signature_count - 1;
    bool has_several_signatures = booter_load.signature_count > 1;
    bool has_signature_for_version = fuse_version <= last_index;
    if (has_several_signatures && !has_signature_for_version) {
        log::error(
            "nvidia: SEC2's fuse register for booter load's ucode id %u gives fuse version %u, while booter load "
            "has signatures only for versions 0 to %u",
            booter_load.ucode_id,
            fuse_version,
            last_index
        );
        return ERR_BAD_FIRMWARE;
    }

    signature_out.fuse_version = fuse_version;
    signature_out.index = has_several_signatures ? last_index - fuse_version : 0;

    return OK;
}

} // namespace drivers::nvidia
