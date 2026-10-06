#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fs/devfs/devfs.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(devfs);

TEST(devfs, append_writes_only_what_fits_and_returns_the_full_length) {
    char buf[4] = {};

    size_t pos = devfs::append_str(buf, sizeof(buf), 0, "hello");
    EXPECT_EQ(pos, 5u);
    EXPECT_EQ(string::memcmp(buf, "hell", 4), 0);

    pos = devfs::append_u64(buf, sizeof(buf), pos, 12345);
    EXPECT_EQ(pos, 10u);
}

static constexpr size_t LARGE_TEXT_LENGTH = 10000;
static constexpr size_t READ_CHUNK_BYTES = 1000;

static char g_large_text[LARGE_TEXT_LENGTH + 1];

static char large_text_letter(size_t index) {
    return static_cast<char>('a' + index % 26);
}

static size_t generate_large_text(char* buf, size_t cap) {
    size_t pos = 0;
    for (size_t i = 0; i < LARGE_TEXT_LENGTH; i++) {
        char letter[2] = { large_text_letter(i), '\0' };
        pos = devfs::append_str(buf, cap, pos, letter);
    }

    return pos;
}

TEST(devfs, a_text_snapshot_grows_to_hold_the_whole_text) {
    int32_t rc = devfs::ERR;
    RUN_ELEVATED({
        fs::node* dir = devfs::ensure_dir("devfs_test");
        auto* node = heap::kalloc_new<devfs::text_snapshot_node>("large", generate_large_text);
        if (dir && node) {
            rc = devfs::add_char_device_at(dir, node);
        }

        if (rc != devfs::OK && node) {
            heap::kfree_delete(node);
        }
    });
    ASSERT_EQ(rc, devfs::OK);

    fs::file* f = fs::open("/dev/devfs_test/large", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    size_t length = 0;
    while (length < sizeof(g_large_text)) {
        size_t room = sizeof(g_large_text) - length;
        size_t chunk = room < READ_CHUNK_BYTES ? room : READ_CHUNK_BYTES;
        ssize_t n = fs::read(f, g_large_text + length, chunk);
        if (n <= 0) {
            break;
        }

        length += static_cast<size_t>(n);
    }

    fs::close(f);

    size_t mismatches = 0;
    for (size_t i = 0; i < length; i++) {
        if (g_large_text[i] != large_text_letter(i)) {
            mismatches++;
        }
    }

    EXPECT_EQ(length, LARGE_TEXT_LENGTH);
    EXPECT_EQ(mismatches, 0u);
}
