#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fw/fw.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"

TEST_SUITE(fw);

// The directories down to the suite's own, in creation order. The suite creates only the missing ones and removes
// exactly those, so an installed package's files are never touched.
static const char* const FIXTURE_DIRECTORIES[] = {
    "/usr",
    "/usr/lib",
    "/usr/lib/firmware",
    "/usr/lib/firmware/fw_test",
};

static constexpr size_t FIXTURE_DIRECTORY_COUNT = sizeof(FIXTURE_DIRECTORIES) / sizeof(FIXTURE_DIRECTORIES[0]);

static bool g_created_directories[FIXTURE_DIRECTORY_COUNT];

// A file every test may open, by its name for `fw::file` and by its path for `fs::`
static constexpr const char FIXTURE_NAME[] = "fw_test/device.bin";
static constexpr const char FIXTURE_PATH[] = "/usr/lib/firmware/fw_test/device.bin";
static constexpr size_t FIXTURE_BYTES = 1237;

// A second file, of a size unlike the first's, so a test can tell which of the two an object holds open
static constexpr const char OTHER_NAME[] = "fw_test/other.bin";
static constexpr const char OTHER_PATH[] = "/usr/lib/firmware/fw_test/other.bin";
static constexpr size_t OTHER_BYTES = 311;

// Larger than any file the tests write
static const uint8_t ZEROS[4096] = {};

static bool write_zeros(const char* path, size_t size) {
    fs::file* f = fs::open(path, fs::O_CREAT | fs::O_TRUNC | fs::O_WRONLY);
    if (!f) {
        return false;
    }

    ssize_t written = fs::write(f, ZEROS, size);
    fs::close(f);

    return written == static_cast<ssize_t>(size);
}

static int32_t remove_fixtures() {
    fs::unlink(FIXTURE_PATH);
    fs::unlink(OTHER_PATH);

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

    bool wrote_files = write_zeros(FIXTURE_PATH, FIXTURE_BYTES) && write_zeros(OTHER_PATH, OTHER_BYTES);
    if (!wrote_files) {
        remove_fixtures();
        return fs::ERR_IO;
    }

    return 0;
}

BEFORE_ALL(fw, create_fixtures);
AFTER_ALL(fw, remove_fixtures);

TEST(fw, file_in_a_missing_directory_is_not_installed) {
    fw::file firmware;
    EXPECT_EQ(firmware.open("fw_test/absent/device.bin"), fw::ERR_NOT_INSTALLED);
}

TEST(fw, file_absent_from_an_existing_directory_is_missing) {
    fw::file firmware;
    EXPECT_EQ(firmware.open("fw_test/absent.bin"), fw::ERR_MISSING);
}

// The file system reports a size for the directory too, which a refused open must not keep
TEST(fw, directory_at_the_name_is_unreadable) {
    fw::file firmware;
    EXPECT_EQ(firmware.open("fw_test"), fw::ERR_UNREADABLE);
    EXPECT_EQ(firmware.size(), 0u);
}

TEST(fw, regular_file_where_a_directory_belongs_is_unreadable) {
    fw::file firmware;
    EXPECT_EQ(firmware.open("fw_test/device.bin/inner.bin"), fw::ERR_UNREADABLE);
}

// Once `fs::` resolves them, the empty name leads to the firmware directory and every other name to the fixture,
// so only the name checks refuse them
static const char* const MALFORMED_NAMES[] = {
    "",
    "/fw_test/device.bin",
    "fw_test/device.bin/",
    "fw_test//device.bin",
    "fw_test/./device.bin",
    "fw_test/../fw_test/device.bin",
};

TEST(fw, malformed_names_are_refused) {
    for (const char* name : MALFORMED_NAMES) {
        fw::file firmware;
        EXPECT_EQ(firmware.open(name), fw::ERR_INVALID_ARG);
    }
}

static constexpr const char TEST_DIRECTORY_PREFIX[] = "fw_test/";

// Room for the test directory's prefix, a component one byte longer than `fs::NAME_MAX` and a NUL
static constexpr size_t COMPONENT_NAME_CAPACITY = sizeof(TEST_DIRECTORY_PREFIX) + fs::NAME_MAX + 1;

static void fill_component_name(char* name, size_t component_length) {
    size_t prefix_length = sizeof(TEST_DIRECTORY_PREFIX) - 1;

    string::memcpy(name, TEST_DIRECTORY_PREFIX, prefix_length);
    string::memset(name + prefix_length, 'a', component_length);
    name[prefix_length + component_length] = '\0';
}

// A component of `fs::NAME_MAX` bytes reaches the file system, which finds no such file in the test directory
TEST(fw, components_longer_than_name_max_are_refused) {
    char name[COMPONENT_NAME_CAPACITY];
    fw::file firmware;

    fill_component_name(name, fs::NAME_MAX);
    EXPECT_EQ(firmware.open(name), fw::ERR_MISSING);

    fill_component_name(name, fs::NAME_MAX + 1);
    EXPECT_EQ(firmware.open(name), fw::ERR_INVALID_ARG);
}

// The 17 bytes of "/usr/lib/firmware", a '/', a name of this length and a NUL fill `fs::PATH_MAX` exactly
static constexpr size_t LONGEST_NAME_BYTES = 4077;

// Fills `name` with `length` bytes of components of `fs::NAME_MAX` bytes or fewer, then a NUL
static void fill_long_name(char* name, size_t length) {
    for (size_t i = 0; i < length; i++) {
        bool ends_component = (i + 1) % (fs::NAME_MAX + 1) == 0;
        name[i] = ends_component ? '/' : 'a';
    }

    name[length] = '\0';
}

// The longest name reaches the file system, which finds no directory of its first component
TEST(fw, names_too_long_for_the_path_limit_are_refused) {
    char* name = static_cast<char*>(heap::ualloc(LONGEST_NAME_BYTES + 2));
    ASSERT_NOT_NULL(name);

    fw::file firmware;
    fill_long_name(name, LONGEST_NAME_BYTES);
    EXPECT_EQ(firmware.open(name), fw::ERR_NOT_INSTALLED);

    fill_long_name(name, LONGEST_NAME_BYTES + 1);
    EXPECT_EQ(firmware.open(name), fw::ERR_INVALID_ARG);

    heap::ufree(name);
}

TEST(fw, size_is_the_file_size_until_close) {
    fw::file firmware;
    ASSERT_EQ(firmware.open(FIXTURE_NAME), fw::OK);
    EXPECT_EQ(firmware.size(), FIXTURE_BYTES);

    firmware.close();
    EXPECT_EQ(firmware.size(), 0u);
}

// A second close must do nothing, so the object can open a file again
TEST(fw, closing_twice_leaves_the_object_reusable) {
    fw::file firmware;
    ASSERT_EQ(firmware.open(FIXTURE_NAME), fw::OK);

    firmware.close();
    firmware.close();
    EXPECT_EQ(firmware.open(FIXTURE_NAME), fw::OK);
    EXPECT_EQ(firmware.size(), FIXTURE_BYTES);
}

TEST(fw, open_on_an_open_object_is_refused_and_keeps_its_file) {
    fw::file firmware;
    ASSERT_EQ(firmware.open(FIXTURE_NAME), fw::OK);
    EXPECT_EQ(firmware.open(OTHER_NAME), fw::ERR_INVALID_ARG);
    EXPECT_EQ(firmware.size(), FIXTURE_BYTES);
}
