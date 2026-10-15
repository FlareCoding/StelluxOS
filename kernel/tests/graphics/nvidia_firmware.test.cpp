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
using drivers::nvidia::booter_layout;
using drivers::nvidia::bootloader_layout;
using drivers::nvidia::check_boot_file;
using drivers::nvidia::check_gsp_image;
using drivers::nvidia::ERR_BAD_FIRMWARE;
using drivers::nvidia::FIRMWARE_VERSION;
using drivers::nvidia::OK;
using drivers::nvidia::read_booter_layout;
using drivers::nvidia::read_bootloader_layout;

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

// No function under test returns it, so a test fails when its file could not be allocated, written or opened, and a
// layout test also fails when the header check refuses its file
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

// What a refused check or read leaves in each field of the caller's header or layout
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

// Where each field of the bootloader's descriptor the driver reads lies, in bytes from the descriptor's start, and
// the bytes up to the last one's end, restated here so the tests hold the read to the format
static constexpr uint32_t BOOTLOADER_APP_VERSION_FIELD         = 0x1C;
static constexpr uint32_t BOOTLOADER_MANIFEST_OFFSET_FIELD     = 0x20;
static constexpr uint32_t BOOTLOADER_MANIFEST_SIZE_FIELD       = 0x24;
static constexpr uint32_t BOOTLOADER_MONITOR_DATA_OFFSET_FIELD = 0x28;
static constexpr uint32_t BOOTLOADER_MONITOR_DATA_SIZE_FIELD   = 0x2C;
static constexpr uint32_t BOOTLOADER_MONITOR_CODE_OFFSET_FIELD = 0x30;
static constexpr uint32_t BOOTLOADER_MONITOR_CODE_SIZE_FIELD   = 0x34;
static constexpr uint32_t BOOTLOADER_DESCRIPTOR_FIELD_BYTES    = 0x38;

struct bootloader_descriptor_values {
    uint32_t app_version;
    uint32_t manifest_offset;
    uint32_t manifest_size;
    uint32_t monitor_data_offset;
    uint32_t monitor_data_size;
    uint32_t monitor_code_offset;
    uint32_t monitor_code_size;
};

// The descriptor of the GA102 bootloader the nvidia-firmware package installs. Each part ends short of the program's
// end, and the manifest offset equals the app version.
static constexpr bootloader_descriptor_values GA102_BOOTLOADER_DESCRIPTOR = {
    .app_version = 0,
    .manifest_offset = 0,
    .manifest_size = 0x800,
    .monitor_data_offset = 0x800,
    .monitor_data_size = 0x1000,
    .monitor_code_offset = 0x1800,
    .monitor_code_size = 0x2900,
};

// Writes `values` into the descriptor of `file`, which starts right after its header
static void put_descriptor(boot_file& file, const bootloader_descriptor_values& values) {
    file.put(BOOT_HEADER_BYTES + BOOTLOADER_APP_VERSION_FIELD, values.app_version);
    file.put(BOOT_HEADER_BYTES + BOOTLOADER_MANIFEST_OFFSET_FIELD, values.manifest_offset);
    file.put(BOOT_HEADER_BYTES + BOOTLOADER_MANIFEST_SIZE_FIELD, values.manifest_size);
    file.put(BOOT_HEADER_BYTES + BOOTLOADER_MONITOR_DATA_OFFSET_FIELD, values.monitor_data_offset);
    file.put(BOOT_HEADER_BYTES + BOOTLOADER_MONITOR_DATA_SIZE_FIELD, values.monitor_data_size);
    file.put(BOOT_HEADER_BYTES + BOOTLOADER_MONITOR_CODE_OFFSET_FIELD, values.monitor_code_offset);
    file.put(BOOT_HEADER_BYTES + BOOTLOADER_MONITOR_CODE_SIZE_FIELD, values.monitor_code_size);
}

// Installs `file`, checks its header and reads its layout into `layout_out`
static int32_t read_layout(const boot_file& file, bootloader_layout& layout_out) {
    fw::file opened;
    if (!file.bytes || !install(file.bytes, file.size, opened)) {
        return NOT_CHECKED;
    }

    boot_file_header header = {};
    if (check_boot_file(opened, "test bootloader", header) != OK) {
        return NOT_CHECKED;
    }

    return read_bootloader_layout(opened, header, layout_out);
}

static int32_t read_layout(const boot_file& file) {
    bootloader_layout layout = {};
    return read_layout(file, layout);
}

// The GA102 bootloader with 256 bytes after its program, so a part that ends past the program can still end inside
// the file
static constexpr boot_file_values PADDED_BOOTLOADER = {
    .file_bytes = GA102_BOOTLOADER.file_bytes + 256,
    .rounded_size = GA102_BOOTLOADER.rounded_size + 256,
    .program_offset = GA102_BOOTLOADER.program_offset,
    .program_size = GA102_BOOTLOADER.program_size,
};

// Reads the layout of the padded bootloader holding the GA102 descriptor with the field at `field` set to `value`
static int32_t read_padded_layout_with(uint32_t field, uint32_t value) {
    boot_file bootloader(PADDED_BOOTLOADER);
    put_descriptor(bootloader, GA102_BOOTLOADER_DESCRIPTOR);
    bootloader.put(BOOT_HEADER_BYTES + field, value);
    return read_layout(bootloader);
}

TEST(nvidia_firmware, ga102_bootloader_layout_is_read) {
    boot_file bootloader(GA102_BOOTLOADER);
    put_descriptor(bootloader, GA102_BOOTLOADER_DESCRIPTOR);

    bootloader_layout layout = {};
    ASSERT_EQ(read_layout(bootloader, layout), OK);

    EXPECT_EQ(layout.manifest_offset, GA102_BOOTLOADER_DESCRIPTOR.manifest_offset);
    EXPECT_EQ(layout.monitor_data_offset, GA102_BOOTLOADER_DESCRIPTOR.monitor_data_offset);
    EXPECT_EQ(layout.monitor_code_offset, GA102_BOOTLOADER_DESCRIPTOR.monitor_code_offset);
    EXPECT_EQ(layout.app_version, GA102_BOOTLOADER_DESCRIPTOR.app_version);
}

// A bootloader whose four fields the read returns all differ and need more than 16 bits, and whose monitor code
// ends at the program's end
static constexpr uint32_t WIDE_PROGRAM_BYTES = 0x30000;

static constexpr boot_file_values WIDE_BOOTLOADER = {
    .file_bytes = GA102_BOOTLOADER.program_offset + WIDE_PROGRAM_BYTES,
    .rounded_size = GA102_BOOTLOADER.program_offset + WIDE_PROGRAM_BYTES,
    .program_offset = GA102_BOOTLOADER.program_offset,
    .program_size = WIDE_PROGRAM_BYTES,
};

static constexpr bootloader_descriptor_values WIDE_BOOTLOADER_DESCRIPTOR = {
    .app_version = 0x8A5B3C2D,
    .manifest_offset = 0x10100,
    .manifest_size = 0x800,
    .monitor_data_offset = 0x11200,
    .monitor_data_size = 0x1000,
    .monitor_code_offset = 0x12400,
    .monitor_code_size = WIDE_PROGRAM_BYTES - 0x12400,
};

TEST(nvidia_firmware, bootloader_layout_holds_each_field_in_full) {
    boot_file bootloader(WIDE_BOOTLOADER);
    put_descriptor(bootloader, WIDE_BOOTLOADER_DESCRIPTOR);

    bootloader_layout layout = {};
    ASSERT_EQ(read_layout(bootloader, layout), OK);

    EXPECT_EQ(layout.manifest_offset, WIDE_BOOTLOADER_DESCRIPTOR.manifest_offset);
    EXPECT_EQ(layout.monitor_data_offset, WIDE_BOOTLOADER_DESCRIPTOR.monitor_data_offset);
    EXPECT_EQ(layout.monitor_code_offset, WIDE_BOOTLOADER_DESCRIPTOR.monitor_code_offset);
    EXPECT_EQ(layout.app_version, WIDE_BOOTLOADER_DESCRIPTOR.app_version);
}

// The GA102 bootloader with its descriptor cut to the bytes up to the end of the last field the driver reads, and its
// program moved up after them
static constexpr boot_file_values SHORTEST_DESCRIPTOR = {
    .file_bytes = GA102_BOOTLOADER.file_bytes,
    .rounded_size = GA102_BOOTLOADER.rounded_size,
    .program_offset = BOOT_HEADER_BYTES + BOOTLOADER_DESCRIPTOR_FIELD_BYTES,
    .program_size = GA102_BOOTLOADER.program_size,
};

TEST(nvidia_firmware, bootloader_descriptor_must_hold_every_field_read) {
    boot_file shortest(SHORTEST_DESCRIPTOR);
    put_descriptor(shortest, GA102_BOOTLOADER_DESCRIPTOR);
    EXPECT_EQ(read_layout(shortest), OK);

    boot_file one_byte_short(SHORTEST_DESCRIPTOR);
    put_descriptor(one_byte_short, GA102_BOOTLOADER_DESCRIPTOR);
    one_byte_short.put(PROGRAM_OFFSET_FIELD, SHORTEST_DESCRIPTOR.program_offset - 1);
    EXPECT_EQ(read_layout(one_byte_short), ERR_BAD_FIRMWARE);
}

// The GA102 bootloader with a descriptor of 0x10010 bytes, whose length cut to 8 or 16 bits is 0x10, short of the
// fields the driver reads
static constexpr uint32_t LONG_DESCRIPTOR_BYTES = 0x10010;

static constexpr boot_file_values LONG_DESCRIPTOR = {
    .file_bytes = BOOT_HEADER_BYTES + LONG_DESCRIPTOR_BYTES + GA102_BOOTLOADER.program_size,
    .rounded_size = BOOT_HEADER_BYTES + LONG_DESCRIPTOR_BYTES + GA102_BOOTLOADER.program_size,
    .program_offset = BOOT_HEADER_BYTES + LONG_DESCRIPTOR_BYTES,
    .program_size = GA102_BOOTLOADER.program_size,
};

TEST(nvidia_firmware, bootloader_descriptor_length_is_computed_past_16_bits) {
    boot_file bootloader(LONG_DESCRIPTOR);
    put_descriptor(bootloader, GA102_BOOTLOADER_DESCRIPTOR);
    EXPECT_EQ(read_layout(bootloader), OK);
}

// The size that takes each part of the GA102 bootloader's program exactly to the program's end
static constexpr uint32_t MANIFEST_TO_PROGRAM_END =
    GA102_BOOTLOADER.program_size - GA102_BOOTLOADER_DESCRIPTOR.manifest_offset;
static constexpr uint32_t MONITOR_DATA_TO_PROGRAM_END =
    GA102_BOOTLOADER.program_size - GA102_BOOTLOADER_DESCRIPTOR.monitor_data_offset;
static constexpr uint32_t MONITOR_CODE_TO_PROGRAM_END =
    GA102_BOOTLOADER.program_size - GA102_BOOTLOADER_DESCRIPTOR.monitor_code_offset;

TEST(nvidia_firmware, bootloader_parts_may_end_at_the_program_end) {
    EXPECT_EQ(read_padded_layout_with(BOOTLOADER_MANIFEST_SIZE_FIELD, MANIFEST_TO_PROGRAM_END), OK);
    EXPECT_EQ(read_padded_layout_with(BOOTLOADER_MONITOR_DATA_SIZE_FIELD, MONITOR_DATA_TO_PROGRAM_END), OK);
    EXPECT_EQ(read_padded_layout_with(BOOTLOADER_MONITOR_CODE_SIZE_FIELD, MONITOR_CODE_TO_PROGRAM_END), OK);
}

// Each part ends inside the file's padding, so only a check against the program refuses it
TEST(nvidia_firmware, bootloader_parts_must_end_inside_the_program) {
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MANIFEST_SIZE_FIELD, MANIFEST_TO_PROGRAM_END + 1),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MONITOR_DATA_SIZE_FIELD, MONITOR_DATA_TO_PROGRAM_END + 1),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MONITOR_CODE_SIZE_FIELD, MONITOR_CODE_TO_PROGRAM_END + 1),
        ERR_BAD_FIRMWARE
    );
}

// Adding this to any of the GA102 descriptor's part fields leaves its low 16 bits as they were, inside the program
static constexpr uint32_t ABOVE_16_BITS = 0x10000;

TEST(nvidia_firmware, bootloader_part_fields_are_read_past_16_bits) {
    const bootloader_descriptor_values& ga102 = GA102_BOOTLOADER_DESCRIPTOR;
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MANIFEST_OFFSET_FIELD, ga102.manifest_offset + ABOVE_16_BITS),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MANIFEST_SIZE_FIELD, ga102.manifest_size + ABOVE_16_BITS),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MONITOR_DATA_OFFSET_FIELD, ga102.monitor_data_offset + ABOVE_16_BITS),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MONITOR_DATA_SIZE_FIELD, ga102.monitor_data_size + ABOVE_16_BITS),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MONITOR_CODE_OFFSET_FIELD, ga102.monitor_code_offset + ABOVE_16_BITS),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MONITOR_CODE_SIZE_FIELD, ga102.monitor_code_size + ABOVE_16_BITS),
        ERR_BAD_FIRMWARE
    );
}

// A size and an offset whose sum with the part's other field wraps around to 0x10 in 32-bit arithmetic
static constexpr uint32_t WRAPPING_MONITOR_CODE_SIZE = 0x10 - GA102_BOOTLOADER_DESCRIPTOR.monitor_code_offset;
static constexpr uint32_t WRAPPING_MANIFEST_OFFSET   = 0x10 - GA102_BOOTLOADER_DESCRIPTOR.manifest_size;

TEST(nvidia_firmware, bootloader_part_bounds_are_checked_without_wrapping) {
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MONITOR_CODE_SIZE_FIELD, WRAPPING_MONITOR_CODE_SIZE),
        ERR_BAD_FIRMWARE
    );
    EXPECT_EQ(
        read_padded_layout_with(BOOTLOADER_MANIFEST_OFFSET_FIELD, WRAPPING_MANIFEST_OFFSET),
        ERR_BAD_FIRMWARE
    );
}

// The monitor code is the last part checked, so every field has been read before the refusal
TEST(nvidia_firmware, refused_bootloader_leaves_the_layout_unchanged) {
    boot_file bootloader(PADDED_BOOTLOADER);
    put_descriptor(bootloader, GA102_BOOTLOADER_DESCRIPTOR);
    bootloader.put(BOOT_HEADER_BYTES + BOOTLOADER_MONITOR_CODE_SIZE_FIELD, MONITOR_CODE_TO_PROGRAM_END + 1);

    bootloader_layout layout = {
        .manifest_offset = UNCHANGED_FIELD,
        .monitor_data_offset = UNCHANGED_FIELD,
        .monitor_code_offset = UNCHANGED_FIELD,
        .app_version = UNCHANGED_FIELD,
    };

    ASSERT_EQ(read_layout(bootloader, layout), ERR_BAD_FIRMWARE);

    EXPECT_EQ(layout.manifest_offset, UNCHANGED_FIELD);
    EXPECT_EQ(layout.monitor_data_offset, UNCHANGED_FIELD);
    EXPECT_EQ(layout.monitor_code_offset, UNCHANGED_FIELD);
    EXPECT_EQ(layout.app_version, UNCHANGED_FIELD);
}

// Cutting the bootloader to its header after opening leaves the descriptor read short, and a zeroed buffer places every
// part inside the program, so only the failed read refuses the file
TEST(nvidia_firmware, bootloader_cut_short_after_opening_is_refused) {
    boot_file bootloader(GA102_BOOTLOADER);
    ASSERT_NOT_NULL(bootloader.bytes);
    put_descriptor(bootloader, GA102_BOOTLOADER_DESCRIPTOR);

    fw::file opened;
    ASSERT_TRUE(install(bootloader.bytes, bootloader.size, opened));

    boot_file_header header = {};
    ASSERT_EQ(check_boot_file(opened, "test bootloader", header), OK);

    fw::file cut_short;
    ASSERT_TRUE(install(bootloader.bytes, BOOT_HEADER_BYTES, cut_short));

    bootloader_layout layout = {};
    EXPECT_EQ(read_bootloader_layout(opened, header, layout), ERR_BAD_FIRMWARE);
}

// Where each field of booter load's descriptor header lies, in bytes from the header's start, and the header's size,
// restated here so the tests hold the read to the format
static constexpr uint32_t BOOTER_SIGNATURES_OFFSET_FIELD      = 0x00;
static constexpr uint32_t BOOTER_SIGNATURES_SIZE_FIELD        = 0x04;
static constexpr uint32_t BOOTER_SLOT_DESCRIPTOR_FIELD        = 0x08;
static constexpr uint32_t BOOTER_PATCH_INDEX_DESCRIPTOR_FIELD = 0x0C;
static constexpr uint32_t BOOTER_METADATA_OFFSET_FIELD        = 0x10;
static constexpr uint32_t BOOTER_METADATA_SIZE_FIELD          = 0x14;
static constexpr uint32_t BOOTER_COUNT_DESCRIPTOR_FIELD       = 0x18;
static constexpr uint32_t BOOTER_LOAD_HEADER_OFFSET_FIELD     = 0x1C;
static constexpr uint32_t BOOTER_LOAD_HEADER_SIZE_FIELD       = 0x20;
static constexpr uint32_t BOOTER_DESCRIPTOR_HEADER_BYTES      = 0x24;

// Bytes of each descriptor that is one 32-bit field: the slot's offset, the signature patch index and the count
static constexpr uint32_t BOOTER_FIELD_DESCRIPTOR_BYTES = 4;

// Where each field of booter load's metadata and load header lies, in bytes from each one's start, and their sizes
static constexpr uint32_t BOOTER_FUSE_VERSION_FIELD    = 0x00;
static constexpr uint32_t BOOTER_ENGINE_MASK_FIELD     = 0x04;
static constexpr uint32_t BOOTER_UCODE_ID_FIELD        = 0x08;
static constexpr uint32_t BOOTER_METADATA_BYTES        = 12;
static constexpr uint32_t BOOTER_OS_CODE_OFFSET_FIELD  = 0x00;
static constexpr uint32_t BOOTER_OS_CODE_SIZE_FIELD    = 0x04;
static constexpr uint32_t BOOTER_DATA_OFFSET_FIELD     = 0x08;
static constexpr uint32_t BOOTER_DATA_SIZE_FIELD       = 0x0C;
static constexpr uint32_t BOOTER_APP_COUNT_FIELD       = 0x10;
static constexpr uint32_t BOOTER_CODE_OFFSET_FIELD     = 0x14;
static constexpr uint32_t BOOTER_CODE_SIZE_FIELD       = 0x18;
static constexpr uint32_t BOOTER_APP_DATA_OFFSET_FIELD = 0x1C;
static constexpr uint32_t BOOTER_APP_DATA_SIZE_FIELD   = 0x20;
static constexpr uint32_t BOOTER_LOAD_HEADER_BYTES     = 36;

// The values booter load's descriptors hold, with the fields the driver does not read last
struct booter_descriptor_values {
    uint32_t signature_size;
    uint32_t signature_count;
    uint32_t signature_slot;
    uint32_t engine_mask;
    uint32_t ucode_id;
    uint32_t code_offset;
    uint32_t code_size;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t fuse_version;
    uint32_t os_code_offset;
    uint32_t os_code_size;
    uint32_t app_data_offset;
    uint32_t app_data_size;
};

// The descriptors of the GA102 booter load the nvidia-firmware package installs. Its data ends at the program's end,
// and its OS code's size equals its code's offset.
static constexpr booter_descriptor_values GA102_BOOTER_DESCRIPTORS = {
    .signature_size = 384,
    .signature_count = 2,
    .signature_slot = 0x8410,
    .engine_mask = 0x1,
    .ucode_id = 3,
    .code_offset = 0x100,
    .code_size = 0x8300,
    .data_offset = 0x8400,
    .data_size = 0x6200,
    .fuse_version = 1,
    .os_code_offset = 0,
    .os_code_size = 0x100,
    .app_data_offset = 0x100,
    .app_data_size = 0,
};

// Where NVIDIA's extraction tool places booter load's descriptors, in bytes from the file's start: the signatures, the
// slot's offset, the signature patch index, the metadata, the count and the load header in turn, then the program
struct booter_places {
    uint32_t signatures;
    uint32_t slot_offset;
    uint32_t patch_index;
    uint32_t metadata;
    uint32_t count;
    uint32_t load_header;
    uint32_t program;
};

static constexpr booter_places place_booter_descriptors(const booter_descriptor_values& values) {
    uint32_t signatures = BOOT_HEADER_BYTES + BOOTER_DESCRIPTOR_HEADER_BYTES;
    uint32_t slot_offset = signatures + values.signature_size * values.signature_count;
    uint32_t patch_index = slot_offset + BOOTER_FIELD_DESCRIPTOR_BYTES;
    uint32_t metadata = patch_index + BOOTER_FIELD_DESCRIPTOR_BYTES;
    uint32_t count = metadata + BOOTER_METADATA_BYTES;
    uint32_t load_header = count + BOOTER_FIELD_DESCRIPTOR_BYTES;

    return {
        .signatures = signatures,
        .slot_offset = slot_offset,
        .patch_index = patch_index,
        .metadata = metadata,
        .count = count,
        .load_header = load_header,
        .program = load_header + BOOTER_LOAD_HEADER_BYTES,
    };
}

static constexpr booter_places GA102_BOOTER_PLACES = place_booter_descriptors(GA102_BOOTER_DESCRIPTORS);

// The tool's places start the GA102 program where the GA102 header does
static_assert(GA102_BOOTER_PLACES.program == GA102_BOOTER_LOAD.program_offset);

// Writes `values` into the descriptors of `file`, placed as `place_booter_descriptors` places them
static void put_booter_descriptors(boot_file& file, const booter_descriptor_values& values) {
    booter_places places = place_booter_descriptors(values);

    file.put(BOOT_HEADER_BYTES + BOOTER_SIGNATURES_OFFSET_FIELD, places.signatures);
    file.put(BOOT_HEADER_BYTES + BOOTER_SIGNATURES_SIZE_FIELD, values.signature_size * values.signature_count);
    file.put(BOOT_HEADER_BYTES + BOOTER_SLOT_DESCRIPTOR_FIELD, places.slot_offset);
    file.put(BOOT_HEADER_BYTES + BOOTER_PATCH_INDEX_DESCRIPTOR_FIELD, places.patch_index);
    file.put(BOOT_HEADER_BYTES + BOOTER_METADATA_OFFSET_FIELD, places.metadata);
    file.put(BOOT_HEADER_BYTES + BOOTER_METADATA_SIZE_FIELD, BOOTER_METADATA_BYTES);
    file.put(BOOT_HEADER_BYTES + BOOTER_COUNT_DESCRIPTOR_FIELD, places.count);
    file.put(BOOT_HEADER_BYTES + BOOTER_LOAD_HEADER_OFFSET_FIELD, places.load_header);
    file.put(BOOT_HEADER_BYTES + BOOTER_LOAD_HEADER_SIZE_FIELD, BOOTER_LOAD_HEADER_BYTES);

    file.put(places.slot_offset, values.signature_slot);
    file.put(places.patch_index, 0);
    file.put(places.metadata + BOOTER_FUSE_VERSION_FIELD, values.fuse_version);
    file.put(places.metadata + BOOTER_ENGINE_MASK_FIELD, values.engine_mask);
    file.put(places.metadata + BOOTER_UCODE_ID_FIELD, values.ucode_id);
    file.put(places.count, values.signature_count);

    file.put(places.load_header + BOOTER_OS_CODE_OFFSET_FIELD, values.os_code_offset);
    file.put(places.load_header + BOOTER_OS_CODE_SIZE_FIELD, values.os_code_size);
    file.put(places.load_header + BOOTER_DATA_OFFSET_FIELD, values.data_offset);
    file.put(places.load_header + BOOTER_DATA_SIZE_FIELD, values.data_size);
    file.put(places.load_header + BOOTER_APP_COUNT_FIELD, 1);
    file.put(places.load_header + BOOTER_CODE_OFFSET_FIELD, values.code_offset);
    file.put(places.load_header + BOOTER_CODE_SIZE_FIELD, values.code_size);
    file.put(places.load_header + BOOTER_APP_DATA_OFFSET_FIELD, values.app_data_offset);
    file.put(places.load_header + BOOTER_APP_DATA_SIZE_FIELD, values.app_data_size);
}

// Installs `file`, checks its header and reads booter load's layout into `layout_out`
static int32_t read_booter(const boot_file& file, booter_layout& layout_out) {
    fw::file opened;
    if (!file.bytes || !install(file.bytes, file.size, opened)) {
        return NOT_CHECKED;
    }

    boot_file_header header = {};
    if (check_boot_file(opened, "test booter load", header) != OK) {
        return NOT_CHECKED;
    }

    return read_booter_layout(opened, header, layout_out);
}

static int32_t read_booter(const boot_file& file) {
    booter_layout layout = {};
    return read_booter(file, layout);
}

// The GA102 booter load with 256 bytes after its program, so a part that ends past the program can still end inside
// the file
static constexpr boot_file_values PADDED_BOOTER_LOAD = {
    .file_bytes = GA102_BOOTER_LOAD.file_bytes + 256,
    .rounded_size = GA102_BOOTER_LOAD.rounded_size + 256,
    .program_offset = GA102_BOOTER_LOAD.program_offset,
    .program_size = GA102_BOOTER_LOAD.program_size,
};

// Reads the layout of the padded booter load holding `values`
static int32_t read_padded_booter(const booter_descriptor_values& values) {
    boot_file booter_load(PADDED_BOOTER_LOAD);
    put_booter_descriptors(booter_load, values);
    return read_booter(booter_load);
}

// Reads the layout of the padded booter load holding the GA102 descriptors with the 32 bits at `offset` in the file
// set to `value`
static int32_t read_padded_booter_with(uint32_t offset, uint32_t value) {
    boot_file booter_load(PADDED_BOOTER_LOAD);
    put_booter_descriptors(booter_load, GA102_BOOTER_DESCRIPTORS);
    booter_load.put(offset, value);
    return read_booter(booter_load);
}

TEST(nvidia_firmware, ga102_booter_load_layout_is_read) {
    boot_file booter_load(GA102_BOOTER_LOAD);
    put_booter_descriptors(booter_load, GA102_BOOTER_DESCRIPTORS);

    booter_layout layout = {};
    ASSERT_EQ(read_booter(booter_load, layout), OK);

    EXPECT_EQ(layout.code_offset, GA102_BOOTER_DESCRIPTORS.code_offset);
    EXPECT_EQ(layout.code_size, GA102_BOOTER_DESCRIPTORS.code_size);
    EXPECT_EQ(layout.data_offset, GA102_BOOTER_DESCRIPTORS.data_offset);
    EXPECT_EQ(layout.data_size, GA102_BOOTER_DESCRIPTORS.data_size);
    EXPECT_EQ(layout.signature_slot, GA102_BOOTER_DESCRIPTORS.signature_slot - GA102_BOOTER_DESCRIPTORS.data_offset);
    EXPECT_EQ(layout.signatures_offset, GA102_BOOTER_PLACES.signatures);
    EXPECT_EQ(layout.signature_size, GA102_BOOTER_DESCRIPTORS.signature_size);
    EXPECT_EQ(layout.signature_count, GA102_BOOTER_DESCRIPTORS.signature_count);
    EXPECT_EQ(layout.ucode_id, GA102_BOOTER_DESCRIPTORS.ucode_id);
    EXPECT_EQ(layout.engine_mask, GA102_BOOTER_DESCRIPTORS.engine_mask);
}

// Descriptors that all hold different values, past 16 bits where the layout returns them but for the signatures, whose
// total size is past 16 bits instead. The code ends 0x80 bytes before the data, and the data at the program's end.
static constexpr booter_descriptor_values WIDE_BOOTER_DESCRIPTORS = {
    .signature_size = 0x8004,
    .signature_count = 2,
    .signature_slot = 0x30310,
    .engine_mask = 0x40001,
    .ucode_id = 0x10005,
    .code_offset = 0x10100,
    .code_size = 0x10180,
    .data_offset = 0x20300,
    .data_size = 0x18500,
    .fuse_version = 0x50006,
    .os_code_offset = 0x60007,
    .os_code_size = 0x70008,
    .app_data_offset = 0x80009,
    .app_data_size = 0x9000A,
};

static constexpr uint32_t WIDE_BOOTER_PROGRAM_OFFSET = place_booter_descriptors(WIDE_BOOTER_DESCRIPTORS).program;
static constexpr uint32_t WIDE_BOOTER_PROGRAM_BYTES  = 0x38800;

static constexpr boot_file_values WIDE_BOOTER_LOAD = {
    .file_bytes = WIDE_BOOTER_PROGRAM_OFFSET + WIDE_BOOTER_PROGRAM_BYTES,
    .rounded_size = WIDE_BOOTER_PROGRAM_OFFSET + WIDE_BOOTER_PROGRAM_BYTES,
    .program_offset = WIDE_BOOTER_PROGRAM_OFFSET,
    .program_size = WIDE_BOOTER_PROGRAM_BYTES,
};

TEST(nvidia_firmware, booter_load_layout_holds_each_field_in_full) {
    boot_file booter_load(WIDE_BOOTER_LOAD);
    put_booter_descriptors(booter_load, WIDE_BOOTER_DESCRIPTORS);

    booter_layout layout = {};
    ASSERT_EQ(read_booter(booter_load, layout), OK);

    EXPECT_EQ(layout.code_offset, WIDE_BOOTER_DESCRIPTORS.code_offset);
    EXPECT_EQ(layout.code_size, WIDE_BOOTER_DESCRIPTORS.code_size);
    EXPECT_EQ(layout.data_offset, WIDE_BOOTER_DESCRIPTORS.data_offset);
    EXPECT_EQ(layout.data_size, WIDE_BOOTER_DESCRIPTORS.data_size);
    EXPECT_EQ(layout.signature_slot, WIDE_BOOTER_DESCRIPTORS.signature_slot - WIDE_BOOTER_DESCRIPTORS.data_offset);
    EXPECT_EQ(layout.signatures_offset, place_booter_descriptors(WIDE_BOOTER_DESCRIPTORS).signatures);
    EXPECT_EQ(layout.signature_size, WIDE_BOOTER_DESCRIPTORS.signature_size);
    EXPECT_EQ(layout.signature_count, WIDE_BOOTER_DESCRIPTORS.signature_count);
    EXPECT_EQ(layout.ucode_id, WIDE_BOOTER_DESCRIPTORS.ucode_id);
    EXPECT_EQ(layout.engine_mask, WIDE_BOOTER_DESCRIPTORS.engine_mask);
}

// The GA102 booter load with its program moved up to start right after the descriptor header, the other descriptors
// still read from where the header leads
static constexpr boot_file_values SHORTEST_BOOTER_DESCRIPTORS = {
    .file_bytes = GA102_BOOTER_LOAD.file_bytes,
    .rounded_size = GA102_BOOTER_LOAD.rounded_size,
    .program_offset = BOOT_HEADER_BYTES + BOOTER_DESCRIPTOR_HEADER_BYTES,
    .program_size = GA102_BOOTER_LOAD.program_size,
};

// The GA102 booter load with 0x10010 bytes of descriptors, whose length cut to 8 or 16 bits is 0x10, short of their
// header
static constexpr boot_file_values LONG_BOOTER_DESCRIPTORS = {
    .file_bytes = BOOT_HEADER_BYTES + LONG_DESCRIPTOR_BYTES + GA102_BOOTER_LOAD.program_size,
    .rounded_size = BOOT_HEADER_BYTES + LONG_DESCRIPTOR_BYTES + GA102_BOOTER_LOAD.program_size,
    .program_offset = BOOT_HEADER_BYTES + LONG_DESCRIPTOR_BYTES,
    .program_size = GA102_BOOTER_LOAD.program_size,
};

TEST(nvidia_firmware, booter_load_descriptors_must_hold_their_header) {
    boot_file shortest(SHORTEST_BOOTER_DESCRIPTORS);
    put_booter_descriptors(shortest, GA102_BOOTER_DESCRIPTORS);
    EXPECT_EQ(read_booter(shortest), OK);

    boot_file one_byte_short(SHORTEST_BOOTER_DESCRIPTORS);
    put_booter_descriptors(one_byte_short, GA102_BOOTER_DESCRIPTORS);
    one_byte_short.put(PROGRAM_OFFSET_FIELD, SHORTEST_BOOTER_DESCRIPTORS.program_offset - 1);
    EXPECT_EQ(read_booter(one_byte_short), ERR_BAD_FIRMWARE);

    boot_file long_descriptors(LONG_BOOTER_DESCRIPTORS);
    put_booter_descriptors(long_descriptors, GA102_BOOTER_DESCRIPTORS);
    EXPECT_EQ(read_booter(long_descriptors), OK);
}

// Each size differs from the one required by 1, or by a bit above the low 16
TEST(nvidia_firmware, booter_load_metadata_and_load_header_must_have_their_sizes) {
    uint32_t metadata_size_field = BOOT_HEADER_BYTES + BOOTER_METADATA_SIZE_FIELD;
    uint32_t load_header_size_field = BOOT_HEADER_BYTES + BOOTER_LOAD_HEADER_SIZE_FIELD;
    uint32_t wide_load_header_size = BOOTER_LOAD_HEADER_BYTES + ABOVE_16_BITS;
    EXPECT_EQ(read_padded_booter_with(metadata_size_field, BOOTER_METADATA_BYTES - 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(metadata_size_field, BOOTER_METADATA_BYTES + 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(metadata_size_field, BOOTER_METADATA_BYTES + ABOVE_16_BITS), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(load_header_size_field, BOOTER_LOAD_HEADER_BYTES - 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(load_header_size_field, BOOTER_LOAD_HEADER_BYTES + 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(load_header_size_field, wide_load_header_size), ERR_BAD_FIRMWARE);
}

// The signature patch index read from the padded file's last 4 bytes, which hold 0 as the index must, and from 1 byte
// later, then each descriptor 0x10000 past its GA102 place, past the file's end while its low 16 bits are unchanged
TEST(nvidia_firmware, booter_load_descriptors_must_lie_inside_the_file) {
    uint32_t last_descriptor = PADDED_BOOTER_LOAD.file_bytes - BOOTER_FIELD_DESCRIPTOR_BYTES;
    uint32_t patch_index_field = BOOT_HEADER_BYTES + BOOTER_PATCH_INDEX_DESCRIPTOR_FIELD;
    EXPECT_EQ(read_padded_booter_with(patch_index_field, last_descriptor), OK);
    EXPECT_EQ(read_padded_booter_with(patch_index_field, last_descriptor + 1), ERR_BAD_FIRMWARE);

    const booter_places& ga102 = GA102_BOOTER_PLACES;
    uint32_t slot_field = BOOT_HEADER_BYTES + BOOTER_SLOT_DESCRIPTOR_FIELD;
    uint32_t metadata_field = BOOT_HEADER_BYTES + BOOTER_METADATA_OFFSET_FIELD;
    uint32_t count_field = BOOT_HEADER_BYTES + BOOTER_COUNT_DESCRIPTOR_FIELD;
    uint32_t load_header_field = BOOT_HEADER_BYTES + BOOTER_LOAD_HEADER_OFFSET_FIELD;
    EXPECT_EQ(read_padded_booter_with(patch_index_field, ga102.patch_index + ABOVE_16_BITS), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(slot_field, ga102.slot_offset + ABOVE_16_BITS), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(metadata_field, ga102.metadata + ABOVE_16_BITS), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(count_field, ga102.count + ABOVE_16_BITS), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(load_header_field, ga102.load_header + ABOVE_16_BITS), ERR_BAD_FIRMWARE);
}

TEST(nvidia_firmware, booter_load_signature_patch_index_must_be_0) {
    EXPECT_EQ(read_padded_booter_with(GA102_BOOTER_PLACES.patch_index, 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(GA102_BOOTER_PLACES.patch_index, ABOVE_16_BITS), ERR_BAD_FIRMWARE);
}

// The GA102 signatures' total size
static constexpr uint32_t GA102_SIGNATURES_BYTES =
    GA102_BOOTER_DESCRIPTORS.signature_size * GA102_BOOTER_DESCRIPTORS.signature_count;

// A count whose low 16 bits are the GA102 count, 2, does not divide the GA102 signatures' size, and one signature of
// one byte is the least NVIDIA's rules accept
TEST(nvidia_firmware, booter_load_signatures_must_split_evenly_into_at_least_one) {
    uint32_t signatures_size_field = BOOT_HEADER_BYTES + BOOTER_SIGNATURES_SIZE_FIELD;
    uint32_t ga102_count = GA102_BOOTER_DESCRIPTORS.signature_count;
    EXPECT_EQ(read_padded_booter_with(GA102_BOOTER_PLACES.count, 0), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(signatures_size_field, 0), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(signatures_size_field, GA102_SIGNATURES_BYTES - 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(GA102_BOOTER_PLACES.count, ga102_count + ABOVE_16_BITS), ERR_BAD_FIRMWARE);

    boot_file one_byte_signature(PADDED_BOOTER_LOAD);
    put_booter_descriptors(one_byte_signature, GA102_BOOTER_DESCRIPTORS);
    one_byte_signature.put(signatures_size_field, 1);
    one_byte_signature.put(GA102_BOOTER_PLACES.count, 1);
    EXPECT_EQ(read_booter(one_byte_signature), OK);
}

// The offset that ends the GA102 signatures at the padded file's end, and one whose sum with their size wraps around
// to 0x10 in 32-bit arithmetic
static constexpr uint32_t SIGNATURES_AT_FILE_END     = PADDED_BOOTER_LOAD.file_bytes - GA102_SIGNATURES_BYTES;
static constexpr uint32_t WRAPPING_SIGNATURES_OFFSET = 0x10 - GA102_SIGNATURES_BYTES;

TEST(nvidia_firmware, booter_load_signatures_must_end_inside_the_file) {
    uint32_t signatures_offset_field = BOOT_HEADER_BYTES + BOOTER_SIGNATURES_OFFSET_FIELD;
    EXPECT_EQ(read_padded_booter_with(signatures_offset_field, SIGNATURES_AT_FILE_END), OK);
    EXPECT_EQ(read_padded_booter_with(signatures_offset_field, SIGNATURES_AT_FILE_END + 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(signatures_offset_field, WRAPPING_SIGNATURES_OFFSET), ERR_BAD_FIRMWARE);
    EXPECT_EQ(
        read_padded_booter_with(signatures_offset_field, GA102_BOOTER_PLACES.signatures + ABOVE_16_BITS),
        ERR_BAD_FIRMWARE
    );
}

TEST(nvidia_firmware, booter_load_load_header_must_list_one_app) {
    uint32_t app_count_field = GA102_BOOTER_PLACES.load_header + BOOTER_APP_COUNT_FIELD;
    EXPECT_EQ(read_padded_booter_with(app_count_field, 0), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(app_count_field, 2), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(app_count_field, 1 + ABOVE_16_BITS), ERR_BAD_FIRMWARE);
}

// The GA102 data already ends at the program's end, and the padding lets either part end past it inside the file
TEST(nvidia_firmware, booter_load_code_and_data_must_end_inside_the_program) {
    uint32_t code_size_field = GA102_BOOTER_PLACES.load_header + BOOTER_CODE_SIZE_FIELD;
    uint32_t data_size_field = GA102_BOOTER_PLACES.load_header + BOOTER_DATA_SIZE_FIELD;
    uint32_t code_to_program_end = GA102_BOOTER_LOAD.program_size - GA102_BOOTER_DESCRIPTORS.code_offset;
    EXPECT_EQ(read_padded_booter_with(code_size_field, code_to_program_end), OK);
    EXPECT_EQ(read_padded_booter_with(code_size_field, code_to_program_end + 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(data_size_field, GA102_BOOTER_DESCRIPTORS.data_size + 1), ERR_BAD_FIRMWARE);
}

// The data size that ends the GA102 data right at its signature slot's end, and a slot whose sum with the signature's
// size wraps around to 0x10 in 32-bit arithmetic
static constexpr uint32_t DATA_TO_SLOT_END = GA102_BOOTER_DESCRIPTORS.signature_slot +
                                             GA102_BOOTER_DESCRIPTORS.signature_size -
                                             GA102_BOOTER_DESCRIPTORS.data_offset;
static constexpr uint32_t WRAPPING_SLOT = 0x10 - GA102_BOOTER_DESCRIPTORS.signature_size;

TEST(nvidia_firmware, booter_load_slot_must_lie_inside_the_data) {
    uint32_t data_size_field = GA102_BOOTER_PLACES.load_header + BOOTER_DATA_SIZE_FIELD;
    EXPECT_EQ(read_padded_booter_with(data_size_field, DATA_TO_SLOT_END), OK);
    EXPECT_EQ(read_padded_booter_with(data_size_field, DATA_TO_SLOT_END - 1), ERR_BAD_FIRMWARE);
    EXPECT_EQ(read_padded_booter_with(GA102_BOOTER_PLACES.slot_offset, WRAPPING_SLOT), ERR_BAD_FIRMWARE);

    booter_descriptor_values data_from_slot = GA102_BOOTER_DESCRIPTORS;
    data_from_slot.data_offset = data_from_slot.signature_slot;
    data_from_slot.data_size = GA102_BOOTER_LOAD.program_size - data_from_slot.data_offset;
    EXPECT_EQ(read_padded_booter(data_from_slot), OK);

    data_from_slot.data_offset++;
    data_from_slot.data_size--;
    EXPECT_EQ(read_padded_booter(data_from_slot), ERR_BAD_FIRMWARE);
}

// The slot that ends at the GA102 program's end, inside the data since the data ends there too
static constexpr uint32_t SLOT_AT_PROGRAM_END =
    GA102_BOOTER_LOAD.program_size - GA102_BOOTER_DESCRIPTORS.signature_size;

// NVIDIA's driver requires more than one signature's size from the slot's start to the program's end, so a slot
// ending at the end is refused
TEST(nvidia_firmware, booter_load_slot_must_end_before_the_program_end) {
    EXPECT_EQ(read_padded_booter_with(GA102_BOOTER_PLACES.slot_offset, SLOT_AT_PROGRAM_END - 1), OK);
    EXPECT_EQ(read_padded_booter_with(GA102_BOOTER_PLACES.slot_offset, SLOT_AT_PROGRAM_END), ERR_BAD_FIRMWARE);
}

// NVIDIA's slot rule is the last check, so every field has been read before the refusal
TEST(nvidia_firmware, refused_booter_load_leaves_the_layout_unchanged) {
    boot_file booter_load(PADDED_BOOTER_LOAD);
    put_booter_descriptors(booter_load, GA102_BOOTER_DESCRIPTORS);
    booter_load.put(GA102_BOOTER_PLACES.slot_offset, SLOT_AT_PROGRAM_END);

    booter_layout layout = {
        .code_offset = UNCHANGED_FIELD,
        .code_size = UNCHANGED_FIELD,
        .data_offset = UNCHANGED_FIELD,
        .data_size = UNCHANGED_FIELD,
        .signature_slot = UNCHANGED_FIELD,
        .signatures_offset = UNCHANGED_FIELD,
        .signature_size = UNCHANGED_FIELD,
        .signature_count = UNCHANGED_FIELD,
        .ucode_id = UNCHANGED_FIELD,
        .engine_mask = UNCHANGED_FIELD,
    };

    ASSERT_EQ(read_booter(booter_load, layout), ERR_BAD_FIRMWARE);

    EXPECT_EQ(layout.code_offset, UNCHANGED_FIELD);
    EXPECT_EQ(layout.code_size, UNCHANGED_FIELD);
    EXPECT_EQ(layout.data_offset, UNCHANGED_FIELD);
    EXPECT_EQ(layout.data_size, UNCHANGED_FIELD);
    EXPECT_EQ(layout.signature_slot, UNCHANGED_FIELD);
    EXPECT_EQ(layout.signatures_offset, UNCHANGED_FIELD);
    EXPECT_EQ(layout.signature_size, UNCHANGED_FIELD);
    EXPECT_EQ(layout.signature_count, UNCHANGED_FIELD);
    EXPECT_EQ(layout.ucode_id, UNCHANGED_FIELD);
    EXPECT_EQ(layout.engine_mask, UNCHANGED_FIELD);
}

// The signature patch index moved to the file's last 4 bytes, which hold 0, then cut off after opening, so only the
// failed read refuses the file
TEST(nvidia_firmware, booter_load_cut_short_after_opening_is_refused) {
    boot_file booter_load(GA102_BOOTER_LOAD);
    ASSERT_NOT_NULL(booter_load.bytes);
    put_booter_descriptors(booter_load, GA102_BOOTER_DESCRIPTORS);
    uint32_t last_descriptor = GA102_BOOTER_LOAD.file_bytes - BOOTER_FIELD_DESCRIPTOR_BYTES;
    booter_load.put(BOOT_HEADER_BYTES + BOOTER_PATCH_INDEX_DESCRIPTOR_FIELD, last_descriptor);

    fw::file opened;
    ASSERT_TRUE(install(booter_load.bytes, booter_load.size, opened));

    boot_file_header header = {};
    ASSERT_EQ(check_boot_file(opened, "test booter load", header), OK);

    booter_layout layout = {};
    ASSERT_EQ(read_booter_layout(opened, header, layout), OK);

    fw::file cut_short;
    ASSERT_TRUE(install(booter_load.bytes, last_descriptor, cut_short));

    EXPECT_EQ(read_booter_layout(opened, header, layout), ERR_BAD_FIRMWARE);
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
