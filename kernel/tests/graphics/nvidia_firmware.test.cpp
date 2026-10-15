// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "exec/elf64.h"
#include "fs/fs.h"
#include "fw/fw.h"
#include "common/string.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_firmware);

using drivers::nvidia::boot_file_header;
using drivers::nvidia::check_boot_file;
using drivers::nvidia::check_gsp_image;
using drivers::nvidia::ERR_BAD_FIRMWARE;
using drivers::nvidia::FIRMWARE_VERSION;
using drivers::nvidia::OK;

// The directories down to the suite's own, in creation order. The suite creates only the missing ones and removes
// exactly those, so an installed package's files are never touched.
static const char* const FIXTURE_DIRECTORIES[] = {
    "/usr",
    "/usr/lib",
    "/usr/lib/firmware",
    "/usr/lib/firmware/nvidia_test",
};

static constexpr size_t FIXTURE_DIRECTORY_COUNT = sizeof(FIXTURE_DIRECTORIES) / sizeof(FIXTURE_DIRECTORIES[0]);

static bool g_created_directories[FIXTURE_DIRECTORY_COUNT];

// Every test writes the file it checks here, by its name for `fw::file` and by its path for `fs::`
static constexpr const char CHECKED_NAME[] = "nvidia_test/checked.bin";
static constexpr const char CHECKED_PATH[] = "/usr/lib/firmware/nvidia_test/checked.bin";

// Neither check returns it, so a test whose file could not be allocated, written or opened fails
static constexpr int32_t NOT_CHECKED = 1;

static int32_t remove_fixtures() {
    fs::unlink(CHECKED_PATH);

    for (size_t i = FIXTURE_DIRECTORY_COUNT; i > 0; i--) {
        if (g_created_directories[i - 1]) {
            fs::rmdir(FIXTURE_DIRECTORIES[i - 1]);
            g_created_directories[i - 1] = false;
        }
    }

    return 0;
}

static int32_t create_fixtures() {
    for (size_t i = 0; i < FIXTURE_DIRECTORY_COUNT; i++) {
        int32_t rc = fs::mkdir(FIXTURE_DIRECTORIES[i], fs::DEFAULT_DIRECTORY_MODE);
        if (rc != fs::OK && rc != fs::ERR_EXIST) {
            remove_fixtures();
            return rc;
        }

        g_created_directories[i] = rc == fs::OK;
    }

    return 0;
}

BEFORE_ALL(nvidia_firmware, create_fixtures);
AFTER_ALL(nvidia_firmware, remove_fixtures);

// Writes `size` bytes as the checked file and opens it into `file_out`
static bool install(const uint8_t* bytes, size_t size, fw::file& file_out) {
    fs::file* f = fs::open(CHECKED_PATH, fs::O_CREAT | fs::O_TRUNC | fs::O_WRONLY);
    if (!f) {
        return false;
    }

    ssize_t written = fs::write(f, bytes, size);
    fs::close(f);

    return written == static_cast<ssize_t>(size) && file_out.open(CHECKED_NAME) == fw::OK;
}

// Where each field of the boot file header lies, and the values every valid header holds, restated here so the
// tests hold the check to the format rather than to its own constants
static constexpr uint32_t MAGIC_FIELD              = 0;
static constexpr uint32_t HEADER_VERSION_FIELD     = 4;
static constexpr uint32_t ROUNDED_SIZE_FIELD       = 8;
static constexpr uint32_t DESCRIPTOR_OFFSET_FIELD  = 12;
static constexpr uint32_t PROGRAM_OFFSET_FIELD     = 16;
static constexpr uint32_t PROGRAM_SIZE_FIELD       = 20;
static constexpr uint32_t BOOT_HEADER_BYTES        = 24;
static constexpr uint32_t BOOT_FILE_MAGIC          = 0x10DE;
static constexpr uint32_t BOOT_FILE_HEADER_VERSION = 1;

struct boot_file_values {
    uint32_t file_bytes;
    uint32_t rounded_size;
    uint32_t program_offset;
    uint32_t program_size;
};

// The lengths and header values of the GA102 boot files the nvidia-firmware package installs. Each program ends at
// its file's end, and each rounded size exceeds its file's length.
static constexpr boot_file_values GA102_BOOTLOADER = {
    .file_bytes = 24684,
    .rounded_size = 24832,
    .program_offset = 108,
    .program_size = 24576,
};

static constexpr boot_file_values GA102_BOOTER_LOAD = {
    .file_bytes = 59768,
    .rounded_size = 59904,
    .program_offset = 888,
    .program_size = 58880,
};

static constexpr boot_file_values GA102_BOOTER_UNLOAD = {
    .file_bytes = 39544,
    .rounded_size = 39680,
    .program_offset = 888,
    .program_size = 38656,
};

// A synthetic boot file: a header holding `values`, then zeros up to the file's length
struct boot_file {
    uint8_t* bytes;
    size_t   size;

    explicit boot_file(const boot_file_values& values);
    ~boot_file() { heap::ufree(bytes); }

    void put(uint32_t field, uint32_t value) {
        if (!bytes) {
            return;
        }

        for (uint32_t i = 0; i < 4; i++) {
            bytes[field + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }
};

boot_file::boot_file(const boot_file_values& values)
    : bytes(static_cast<uint8_t*>(heap::uzalloc(values.file_bytes))), size(values.file_bytes) {
    if (!bytes) {
        return;
    }

    put(MAGIC_FIELD, BOOT_FILE_MAGIC);
    put(HEADER_VERSION_FIELD, BOOT_FILE_HEADER_VERSION);
    put(ROUNDED_SIZE_FIELD, values.rounded_size);
    put(DESCRIPTOR_OFFSET_FIELD, BOOT_HEADER_BYTES);
    put(PROGRAM_OFFSET_FIELD, values.program_offset);
    put(PROGRAM_SIZE_FIELD, values.program_size);
}

// Installs `file` and checks its header into `header_out`
static int32_t check_boot(const boot_file& file, boot_file_header& header_out) {
    fw::file opened;
    if (!file.bytes || !install(file.bytes, file.size, opened)) {
        return NOT_CHECKED;
    }

    return check_boot_file(opened, "test boot file", header_out);
}

static int32_t check_boot(const boot_file& file) {
    boot_file_header header = {};
    return check_boot(file, header);
}

TEST(nvidia_firmware, ga102_boot_files_pass) {
    boot_file bootloader(GA102_BOOTLOADER);
    EXPECT_EQ(check_boot(bootloader), OK);

    boot_file booter_load(GA102_BOOTER_LOAD);
    EXPECT_EQ(check_boot(booter_load), OK);

    boot_file booter_unload(GA102_BOOTER_UNLOAD);
    EXPECT_EQ(check_boot(booter_unload), OK);
}

// A file of only its header, whose empty program starts right at the descriptors, and a file whose program starts
// there and fills the rest of the file
static constexpr boot_file_values HEADER_ONLY = {
    .file_bytes = BOOT_HEADER_BYTES,
    .rounded_size = 256,
    .program_offset = BOOT_HEADER_BYTES,
    .program_size = 0,
};

static constexpr boot_file_values PROGRAM_RIGHT_AFTER_HEADER = {
    .file_bytes = GA102_BOOTLOADER.file_bytes,
    .rounded_size = GA102_BOOTLOADER.rounded_size,
    .program_offset = BOOT_HEADER_BYTES,
    .program_size = GA102_BOOTLOADER.file_bytes - BOOT_HEADER_BYTES,
};

TEST(nvidia_firmware, boot_file_checks_accept_their_exact_limits) {
    boot_file header_only(HEADER_ONLY);
    EXPECT_EQ(check_boot(header_only), OK);

    boot_file program_right_after_header(PROGRAM_RIGHT_AFTER_HEADER);
    EXPECT_EQ(check_boot(program_right_after_header), OK);
}

TEST(nvidia_firmware, boot_file_shorter_than_its_header_is_refused) {
    boot_file file(HEADER_ONLY);
    file.size = BOOT_HEADER_BYTES - 1;
    EXPECT_EQ(check_boot(file), ERR_BAD_FIRMWARE);
}

// The first 20 bytes of a valid header: every field but the program's size, which a short read leaves at 0 and the
// end check accepts, so only the failed read refuses the file
static constexpr size_t CUT_SHORT_BYTES = PROGRAM_SIZE_FIELD;

TEST(nvidia_firmware, boot_file_cut_short_after_opening_is_refused) {
    boot_file bootloader(GA102_BOOTLOADER);
    ASSERT_NOT_NULL(bootloader.bytes);

    fw::file opened;
    ASSERT_TRUE(install(bootloader.bytes, bootloader.size, opened));

    fw::file cut_short;
    ASSERT_TRUE(install(bootloader.bytes, CUT_SHORT_BYTES, cut_short));

    boot_file_header header = {};
    EXPECT_EQ(check_boot_file(opened, "test boot file", header), ERR_BAD_FIRMWARE);
}

TEST(nvidia_firmware, boot_file_must_carry_the_header_magic_and_version) {
    boot_file wrong_magic(GA102_BOOTLOADER);
    wrong_magic.put(MAGIC_FIELD, BOOT_FILE_MAGIC + 1);
    EXPECT_EQ(check_boot(wrong_magic), ERR_BAD_FIRMWARE);

    boot_file wrong_version(GA102_BOOTLOADER);
    wrong_version.put(HEADER_VERSION_FIELD, BOOT_FILE_HEADER_VERSION + 1);
    EXPECT_EQ(check_boot(wrong_version), ERR_BAD_FIRMWARE);
}

// The program still ends inside the file in each, so each breaks only the start of the descriptors or of the program
TEST(nvidia_firmware, boot_file_descriptors_and_program_must_follow_the_header) {
    boot_file early_descriptors(GA102_BOOTLOADER);
    early_descriptors.put(DESCRIPTOR_OFFSET_FIELD, BOOT_HEADER_BYTES - 1);
    EXPECT_EQ(check_boot(early_descriptors), ERR_BAD_FIRMWARE);

    boot_file late_descriptors(GA102_BOOTLOADER);
    late_descriptors.put(DESCRIPTOR_OFFSET_FIELD, BOOT_HEADER_BYTES + 1);
    EXPECT_EQ(check_boot(late_descriptors), ERR_BAD_FIRMWARE);

    boot_file program_in_header(PROGRAM_RIGHT_AFTER_HEADER);
    program_in_header.put(PROGRAM_OFFSET_FIELD, BOOT_HEADER_BYTES - 1);
    EXPECT_EQ(check_boot(program_in_header), ERR_BAD_FIRMWARE);
}

// 108 plus this size wraps around to 57 in 32-bit arithmetic, inside the file
static constexpr uint32_t WRAPPING_PROGRAM_SIZE = 0xFFFFFFFF - 50;

TEST(nvidia_firmware, boot_file_program_must_end_inside_the_file) {
    boot_file one_byte_past(GA102_BOOTLOADER);
    one_byte_past.put(PROGRAM_SIZE_FIELD, GA102_BOOTLOADER.program_size + 1);
    EXPECT_EQ(check_boot(one_byte_past), ERR_BAD_FIRMWARE);

    boot_file wrapping(GA102_BOOTLOADER);
    wrapping.put(PROGRAM_SIZE_FIELD, WRAPPING_PROGRAM_SIZE);
    EXPECT_EQ(check_boot(wrapping), ERR_BAD_FIRMWARE);
}

TEST(nvidia_firmware, boot_file_header_is_returned_once_checked) {
    boot_file bootloader(GA102_BOOTLOADER);
    boot_file_header header = {};
    ASSERT_EQ(check_boot(bootloader, header), OK);

    EXPECT_EQ(header.magic, BOOT_FILE_MAGIC);
    EXPECT_EQ(header.header_version, BOOT_FILE_HEADER_VERSION);
    EXPECT_EQ(header.rounded_size, GA102_BOOTLOADER.rounded_size);
    EXPECT_EQ(header.descriptor_offset, BOOT_HEADER_BYTES);
    EXPECT_EQ(header.program_offset, GA102_BOOTLOADER.program_offset);
    EXPECT_EQ(header.program_size, GA102_BOOTLOADER.program_size);
}

// What a refused check leaves in each field of the caller's header
static constexpr uint32_t UNCHANGED_FIELD = 0xA5A5A5A5;

// The program's end is the last check, so the header has been read and checked in full before the refusal
TEST(nvidia_firmware, refused_boot_file_leaves_the_header_unchanged) {
    boot_file one_byte_past(GA102_BOOTLOADER);
    one_byte_past.put(PROGRAM_SIZE_FIELD, GA102_BOOTLOADER.program_size + 1);

    boot_file_header header = {
        .magic = UNCHANGED_FIELD,
        .header_version = UNCHANGED_FIELD,
        .rounded_size = UNCHANGED_FIELD,
        .descriptor_offset = UNCHANGED_FIELD,
        .program_offset = UNCHANGED_FIELD,
        .program_size = UNCHANGED_FIELD,
    };

    ASSERT_EQ(check_boot(one_byte_past, header), ERR_BAD_FIRMWARE);

    EXPECT_EQ(header.magic, UNCHANGED_FIELD);
    EXPECT_EQ(header.header_version, UNCHANGED_FIELD);
    EXPECT_EQ(header.rounded_size, UNCHANGED_FIELD);
    EXPECT_EQ(header.descriptor_offset, UNCHANGED_FIELD);
    EXPECT_EQ(header.program_offset, UNCHANGED_FIELD);
    EXPECT_EQ(header.program_size, UNCHANGED_FIELD);
}

// Room for 65 section headers and every table a layout places beside them
static constexpr size_t GSP_IMAGE_BYTES = 8192;

// The name table of every synthetic image. .fwversion comes last, so the table ends with its NUL.
static constexpr char NAME_TABLE[] = "\0.shstrtab\0.fwimage\0.fwversion";

// Where each name starts in the table, the value a section header holds to name its section
static constexpr uint32_t SHSTRTAB_NAME_OFFSET  = 1;
static constexpr uint32_t FWIMAGE_NAME_OFFSET   = 11;
static constexpr uint32_t FWVERSION_NAME_OFFSET = 20;

// Where a synthetic GSP image places its parts, as offsets from the file's start and section indexes
struct gsp_layout {
    uint64_t section_table_offset;
    uint16_t section_count;
    uint16_t name_table_index;
    uint64_t name_table_offset;
    uint16_t version_index;
    uint64_t version_offset;
};

// The synthetic GSP image every test checks, rebuilt by `build_image` before each case
alignas(elf64::Shdr) static uint8_t g_image[GSP_IMAGE_BYTES];
static gsp_layout g_layout;

static elf64::Ehdr& image_header() {
    return *reinterpret_cast<elf64::Ehdr*>(g_image);
}

static elf64::Shdr& image_section(uint16_t index) {
    uint64_t offset = g_layout.section_table_offset + index * sizeof(elf64::Shdr);
    return *reinterpret_cast<elf64::Shdr*>(g_image + offset);
}

// Lays out a valid image as `layout` says, with section 0 unnamed and the others named .fwimage unless they are the
// name table or .fwversion. It stays valid until the test breaks one property.
static void build_image(const gsp_layout& layout) {
    string::memset(g_image, 0, sizeof(g_image));
    g_layout = layout;

    elf64::Ehdr& header = image_header();
    header.e_ident[0] = elf64::ELFMAG0;
    header.e_ident[1] = elf64::ELFMAG1;
    header.e_ident[2] = elf64::ELFMAG2;
    header.e_ident[3] = elf64::ELFMAG3;
    header.e_ident[elf64::EI_CLASS] = elf64::ELFCLASS64;
    header.e_ident[elf64::EI_DATA] = elf64::ELFDATA2LSB;
    header.e_shoff = layout.section_table_offset;
    header.e_shentsize = sizeof(elf64::Shdr);
    header.e_shnum = layout.section_count;
    header.e_shstrndx = layout.name_table_index;

    for (uint16_t i = 1; i < layout.section_count; i++) {
        image_section(i).sh_name = FWIMAGE_NAME_OFFSET;
    }

    elf64::Shdr& name_table = image_section(layout.name_table_index);
    name_table.sh_name = SHSTRTAB_NAME_OFFSET;
    name_table.sh_offset = layout.name_table_offset;
    name_table.sh_size = sizeof(NAME_TABLE);
    string::memcpy(g_image + layout.name_table_offset, NAME_TABLE, sizeof(NAME_TABLE));

    elf64::Shdr& version_section = image_section(layout.version_index);
    version_section.sh_name = FWVERSION_NAME_OFFSET;
    version_section.sh_offset = layout.version_offset;
    version_section.sh_size = sizeof(FIRMWARE_VERSION);
    string::memcpy(g_image + layout.version_offset, FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION));
}

static int32_t check_image() {
    fw::file file;
    if (!install(g_image, GSP_IMAGE_BYTES, file)) {
        return NOT_CHECKED;
    }

    return check_gsp_image(file);
}

// As gsp_ga10x.bin of firmware 535.183.01 places them: the section table ends the file, the name table is the last
// of 10 sections, and .fwversion is the third
static constexpr gsp_layout GSP_GA10X_LAYOUT = {
    .section_table_offset = GSP_IMAGE_BYTES - 10 * sizeof(elf64::Shdr),
    .section_count = 10,
    .name_table_index = 9,
    .name_table_offset = 7000,
    .version_index = 2,
    .version_offset = 4000,
};

// The section table in the middle with room for one header after it, the name table's header at index 1 and its
// names right after the ELF header, and .fwversion's contents at an odd offset, ending the file
static constexpr gsp_layout MIDDLE_TABLE_LAYOUT = {
    .section_table_offset = 2048,
    .section_count = 10,
    .name_table_index = 1,
    .name_table_offset = sizeof(elf64::Ehdr),
    .version_index = 7,
    .version_offset = GSP_IMAGE_BYTES - sizeof(FIRMWARE_VERSION),
};

// The most sections an image may have, with room for one more header after the table, and the name table ending
// the file
static constexpr gsp_layout MOST_SECTIONS_LAYOUT = {
    .section_table_offset = sizeof(elf64::Ehdr),
    .section_count = 64,
    .name_table_index = 63,
    .name_table_offset = GSP_IMAGE_BYTES - sizeof(NAME_TABLE),
    .version_index = 33,
    .version_offset = 4800,
};

// The section table ending the file with .fwversion's header last
static constexpr gsp_layout VERSION_LAST_LAYOUT = {
    .section_table_offset = GSP_IMAGE_BYTES - 10 * sizeof(elf64::Shdr),
    .section_count = 10,
    .name_table_index = 1,
    .name_table_offset = 3000,
    .version_index = 9,
    .version_offset = 5001,
};

static constexpr gsp_layout VALID_LAYOUTS[] = {
    GSP_GA10X_LAYOUT,
    MIDDLE_TABLE_LAYOUT,
    MOST_SECTIONS_LAYOUT,
    VERSION_LAST_LAYOUT,
};

TEST(nvidia_firmware, gsp_images_of_varied_layouts_pass) {
    for (const gsp_layout& layout : VALID_LAYOUTS) {
        build_image(layout);
        EXPECT_EQ(check_image(), OK);
    }
}

// Reading the ELF header would refuse this file too, so the case shows only that it is refused, not which check does
TEST(nvidia_firmware, gsp_image_shorter_than_an_elf_header_is_refused) {
    build_image(GSP_GA10X_LAYOUT);

    fw::file file;
    ASSERT_TRUE(install(g_image, sizeof(elf64::Ehdr) - 1, file));

    EXPECT_EQ(check_gsp_image(file), ERR_BAD_FIRMWARE);
}

// Each valid layout ends with a zero byte some read needs: a section header's last, or the NUL of .fwversion's name
// or contents. Cut off after opening, that read comes up short into zeroed memory, so only a failed read refuses.
TEST(nvidia_firmware, gsp_image_cut_short_after_opening_is_refused) {
    for (const gsp_layout& layout : VALID_LAYOUTS) {
        build_image(layout);

        fw::file opened;
        ASSERT_TRUE(install(g_image, GSP_IMAGE_BYTES, opened));

        fw::file cut_short;
        ASSERT_TRUE(install(g_image, GSP_IMAGE_BYTES - 1, cut_short));

        EXPECT_EQ(check_gsp_image(opened), ERR_BAD_FIRMWARE);
    }
}

static constexpr size_t ELF_MAGIC_BYTES = 4;

// ELF's values for a 32-bit class and for big-endian data
static constexpr uint8_t ELFCLASS32  = 1;
static constexpr uint8_t ELFDATA2MSB = 2;

TEST(nvidia_firmware, gsp_image_must_be_a_64_bit_little_endian_elf_file) {
    for (size_t i = 0; i < ELF_MAGIC_BYTES; i++) {
        build_image(GSP_GA10X_LAYOUT);
        image_header().e_ident[i] ^= 0xFF;
        EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
    }

    build_image(GSP_GA10X_LAYOUT);
    image_header().e_ident[elf64::EI_CLASS] = ELFCLASS32;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_header().e_ident[elf64::EI_DATA] = ELFDATA2MSB;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// The check reads 64-byte headers whatever the file says, so only the entry size check refuses these
TEST(nvidia_firmware, gsp_image_section_headers_must_be_64_bytes) {
    build_image(GSP_GA10X_LAYOUT);
    image_header().e_shentsize = sizeof(elf64::Shdr) - 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_header().e_shentsize = sizeof(elf64::Shdr) + 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// With no sections the name table index lies past them. The 65th header is all zeros, a section with an empty name,
// so only the section limit refuses that image.
TEST(nvidia_firmware, gsp_image_with_no_sections_or_more_than_64_is_refused) {
    build_image(GSP_GA10X_LAYOUT);
    image_header().e_shnum = 0;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(MOST_SECTIONS_LAYOUT);
    image_header().e_shnum = MOST_SECTIONS_LAYOUT.section_count + 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

static constexpr uint64_t LAST_OFFSET = 0xFFFFFFFFFFFFFFFFULL;

// The table one byte past the file's end, and at an offset whose end wraps around to 576, inside the file. Reading a
// header past the end would refuse both too, so these show only that the image is refused, not which check does.
TEST(nvidia_firmware, gsp_image_section_table_must_lie_inside_the_file) {
    build_image(GSP_GA10X_LAYOUT);
    image_header().e_shoff += 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_header().e_shoff = LAST_OFFSET - sizeof(elf64::Shdr) + 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// Each index leads to a valid copy of the name table's header, in the null section or just past the table, so only
// the index check refuses them
TEST(nvidia_firmware, gsp_image_name_table_index_must_not_be_0_or_past_the_last_section) {
    build_image(GSP_GA10X_LAYOUT);
    image_section(0) = image_section(GSP_GA10X_LAYOUT.name_table_index);
    image_header().e_shstrndx = 0;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    uint16_t past_last = MIDDLE_TABLE_LAYOUT.section_count;
    build_image(MIDDLE_TABLE_LAYOUT);
    image_section(past_last) = image_section(MIDDLE_TABLE_LAYOUT.name_table_index);
    image_header().e_shstrndx = past_last;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// The table one byte longer than the file holds, and so long that its end wraps around to 1. Every name read stays
// inside the real table, so only the bounds check refuses them.
TEST(nvidia_firmware, gsp_image_name_table_must_lie_inside_the_file) {
    build_image(MOST_SECTIONS_LAYOUT);
    image_section(MOST_SECTIONS_LAYOUT.name_table_index).sh_size += 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.name_table_index).sh_size = LAST_OFFSET - GSP_GA10X_LAYOUT.name_table_offset + 2;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// No section named .fwversion, which the version checks would refuse as well, one named .fwversionX, which only a
// comparison that includes the NUL refuses, and two identical ones
TEST(nvidia_firmware, gsp_image_must_have_exactly_one_version_section) {
    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.version_index).sh_name = FWIMAGE_NAME_OFFSET;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    g_image[GSP_GA10X_LAYOUT.name_table_offset + sizeof(NAME_TABLE) - 1] = 'X';
    image_section(GSP_GA10X_LAYOUT.name_table_index).sh_size = sizeof(NAME_TABLE) + 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.version_index + 1) = image_section(GSP_GA10X_LAYOUT.version_index);
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// The table ending just before .fwversion's NUL, which the zero byte after the table would supply to a check reading
// past its end, and the table ending a byte before the name starts, which only the check of the name's offset refuses
TEST(nvidia_firmware, gsp_image_version_section_name_must_end_inside_the_name_table) {
    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.name_table_index).sh_size = sizeof(NAME_TABLE) - 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.name_table_index).sh_size = FWVERSION_NAME_OFFSET - 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// The contents one byte past the file's end, and at an offset whose end wraps around to 1. Reading the contents would
// refuse both too, so these show only that the image is refused, not which check does.
TEST(nvidia_firmware, gsp_image_version_section_must_lie_inside_the_file) {
    build_image(MIDDLE_TABLE_LAYOUT);
    image_section(MIDDLE_TABLE_LAYOUT.version_index).sh_offset += 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.version_index).sh_offset = LAST_OFFSET - sizeof(FIRMWARE_VERSION) + 2;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// Another version of the same length and one a digit longer, which only comparing the versions up to their NULs
// refuses, then the version without its NUL and with a byte after it
TEST(nvidia_firmware, gsp_image_version_section_must_hold_exactly_the_driver_version_and_its_nul) {
    build_image(GSP_GA10X_LAYOUT);
    g_image[GSP_GA10X_LAYOUT.version_offset] += 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    g_image[GSP_GA10X_LAYOUT.version_offset + sizeof(FIRMWARE_VERSION) - 1] = '0';
    image_section(GSP_GA10X_LAYOUT.version_index).sh_size = sizeof(FIRMWARE_VERSION) + 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.version_index).sh_size = sizeof(FIRMWARE_VERSION) - 1;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);

    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.version_index).sh_size = sizeof(FIRMWARE_VERSION) + 1;
    g_image[GSP_GA10X_LAYOUT.version_offset + sizeof(FIRMWARE_VERSION)] = 'X';
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}

// A section running to the file's end, refused unread. Without the size check, reading it would overrun the check's
// version buffer by kilobytes rather than return a result.
TEST(nvidia_firmware, gsp_image_version_section_larger_than_the_version_buffer_is_refused) {
    build_image(GSP_GA10X_LAYOUT);
    image_section(GSP_GA10X_LAYOUT.version_index).sh_size = GSP_IMAGE_BYTES - GSP_GA10X_LAYOUT.version_offset;
    EXPECT_EQ(check_image(), ERR_BAD_FIRMWARE);
}
