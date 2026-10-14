// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "fs/fs.h"
#include "fw/fw.h"
#include "common/string.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_firmware);

using drivers::nvidia::check_boot_file;
using drivers::nvidia::ERR_BAD_FIRMWARE;
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

// `check_boot_file` never returns it, so a test whose file could not be allocated, written or opened fails
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

static int32_t check_boot(const boot_file& file) {
    fw::file opened;
    if (!file.bytes || !install(file.bytes, file.size, opened)) {
        return NOT_CHECKED;
    }

    return check_boot_file(opened, "test boot file");
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

    EXPECT_EQ(check_boot_file(opened, "test boot file"), ERR_BAD_FIRMWARE);
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
