#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fw/fw.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "common/string.h"
#include "mm/heap.h"
#include "rc/strong_ref.h"

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

// A file of several megabytes, so reads reach offsets far past its first pages
static constexpr const char LARGE_NAME[] = "fw_test/large.bin";
static constexpr const char LARGE_PATH[] = "/usr/lib/firmware/fw_test/large.bin";
static constexpr size_t LARGE_BYTES = 3 * 1024 * 1024 + 4099;

// A file the shrinking test writes, removed by the suite as well in case the test stops early
static constexpr const char SHRUNK_NAME[] = "fw_test/shrunk.bin";
static constexpr const char SHRUNK_PATH[] = "/usr/lib/firmware/fw_test/shrunk.bin";

// Larger than any file the tests write with `write_zeros`
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

// The byte the large file holds at `offset`. Bytes a page or 64 KiB apart differ, so a read from the wrong place
// returns other bytes.
static uint8_t pattern_byte(uint64_t offset) {
    return static_cast<uint8_t>(offset ^ (offset >> 8) ^ (offset >> 16));
}

static constexpr size_t PATTERN_CHUNK_BYTES = 512;

static bool write_pattern(const char* path, size_t size) {
    fs::file* f = fs::open(path, fs::O_CREAT | fs::O_TRUNC | fs::O_WRONLY);
    if (!f) {
        return false;
    }

    uint8_t chunk[PATTERN_CHUNK_BYTES];
    size_t written = 0;
    while (written < size) {
        size_t chunk_bytes = size - written < PATTERN_CHUNK_BYTES ? size - written : PATTERN_CHUNK_BYTES;
        for (size_t i = 0; i < chunk_bytes; i++) {
            chunk[i] = pattern_byte(written + i);
        }

        if (fs::write(f, chunk, chunk_bytes) != static_cast<ssize_t>(chunk_bytes)) {
            break;
        }

        written += chunk_bytes;
    }

    fs::close(f);

    return written == size;
}

static int32_t remove_fixtures() {
    fs::unlink(FIXTURE_PATH);
    fs::unlink(OTHER_PATH);
    fs::unlink(LARGE_PATH);
    fs::unlink(SHRUNK_PATH);

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

    bool wrote_files = write_zeros(FIXTURE_PATH, FIXTURE_BYTES)
        && write_zeros(OTHER_PATH, OTHER_BYTES)
        && write_pattern(LARGE_PATH, LARGE_BYTES);
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

// An open object holds one reference on the file's node, which closing and destroying it give back
TEST(fw, close_and_destruction_release_the_file) {
    fs::node* raw_node = nullptr;
    ASSERT_EQ(fs::lookup(FIXTURE_PATH, &raw_node), fs::OK);
    rc::strong_ref<fs::node> node = rc::strong_ref<fs::node>::adopt(raw_node);
    uint32_t closed_refs = node->ref_count();

    {
        fw::file firmware;
        EXPECT_EQ(firmware.open(FIXTURE_NAME), fw::OK);
        EXPECT_EQ(node->ref_count(), closed_refs + 1);

        firmware.close();
        EXPECT_EQ(node->ref_count(), closed_refs);

        EXPECT_EQ(firmware.open(FIXTURE_NAME), fw::OK);
    }

    EXPECT_EQ(node->ref_count(), closed_refs);
}

struct read_range {
    uint64_t offset;
    size_t   count;
};

static constexpr size_t LONGEST_READ_BYTES = 8195;

// The file's start, a range across pages just past 1 MiB at an odd offset, and its last bytes, all of odd length
static constexpr read_range LARGE_FILE_READS[] = {
    {.offset = 0, .count = 4097},
    {.offset = 1048579, .count = LONGEST_READ_BYTES},
    {.offset = LARGE_BYTES - 777, .count = 777},
};

// Past each read's end, the destination holds a guard that the read must leave untouched
static constexpr size_t READ_GUARD_BYTES = 64;
static constexpr uint8_t READ_GUARD_BYTE = 0xA5;

static bool holds_pattern(const uint8_t* bytes, uint64_t offset, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] != pattern_byte(offset + i)) {
            return false;
        }
    }

    return true;
}

static bool holds_guard(const uint8_t* bytes) {
    for (size_t i = 0; i < READ_GUARD_BYTES; i++) {
        if (bytes[i] != READ_GUARD_BYTE) {
            return false;
        }
    }

    return true;
}

TEST(fw, reads_copy_exactly_the_requested_range) {
    uint8_t* buffer = static_cast<uint8_t*>(heap::ualloc(LONGEST_READ_BYTES + READ_GUARD_BYTES));
    ASSERT_NOT_NULL(buffer);

    fw::file firmware;
    EXPECT_EQ(firmware.open(LARGE_NAME), fw::OK);
    EXPECT_EQ(firmware.size(), LARGE_BYTES);

    for (const read_range& range : LARGE_FILE_READS) {
        string::memset(buffer, READ_GUARD_BYTE, LONGEST_READ_BYTES + READ_GUARD_BYTES);

        EXPECT_EQ(firmware.read(range.offset, buffer, range.count), fw::OK);
        EXPECT_TRUE(holds_pattern(buffer, range.offset, range.count));
        EXPECT_TRUE(holds_guard(buffer + range.count));
    }

    heap::ufree(buffer);
}

static constexpr uint64_t LAST_OFFSET = 0xFFFFFFFFFFFFFFFFULL;

// A range may end exactly at the file's end, while one byte further, or an end that wraps around, is refused
TEST(fw, ranges_past_the_end_of_the_file_are_refused) {
    uint8_t bytes[4] = {};
    fw::file firmware;
    ASSERT_EQ(firmware.open(FIXTURE_NAME), fw::OK);

    EXPECT_EQ(firmware.read(FIXTURE_BYTES - 4, bytes, 4), fw::OK);
    EXPECT_EQ(firmware.read(FIXTURE_BYTES, bytes, 0), fw::OK);
    EXPECT_EQ(firmware.read(FIXTURE_BYTES - 3, bytes, 4), fw::ERR_INVALID_ARG);
    EXPECT_EQ(firmware.read(LAST_OFFSET - 1, bytes, 4), fw::ERR_INVALID_ARG);
}

// A read of no bytes at offset 0 lies inside any file, so it is refused only because the object holds no file open
TEST(fw, reads_without_an_open_file_are_refused) {
    uint8_t byte = 0;
    fw::file firmware;
    EXPECT_EQ(firmware.read(0, &byte, 0), fw::ERR_INVALID_ARG);

    ASSERT_EQ(firmware.open(FIXTURE_NAME), fw::OK);
    firmware.close();
    EXPECT_EQ(firmware.read(0, &byte, 0), fw::ERR_INVALID_ARG);
}

static constexpr size_t SHRUNK_BYTES_AT_OPEN = 600;
static constexpr size_t SHRUNK_BYTES = 100;

// `size()` keeps the size at open, so a range inside it may reach past the file's end once the file shrinks
TEST(fw, reads_past_the_end_of_a_shrunk_file_are_unreadable) {
    ASSERT_TRUE(write_zeros(SHRUNK_PATH, SHRUNK_BYTES_AT_OPEN));

    fw::file firmware;
    ASSERT_EQ(firmware.open(SHRUNK_NAME), fw::OK);
    ASSERT_TRUE(write_zeros(SHRUNK_PATH, SHRUNK_BYTES));

    uint8_t bytes[SHRUNK_BYTES];
    EXPECT_EQ(firmware.read(0, bytes, SHRUNK_BYTES), fw::OK);
    EXPECT_EQ(firmware.read(SHRUNK_BYTES / 2, bytes, SHRUNK_BYTES), fw::ERR_UNREADABLE);

    firmware.close();
    fs::unlink(SHRUNK_PATH);
}
