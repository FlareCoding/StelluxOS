#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/node.h"
#include "mm/mm.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/page_quarantine.h"

TEST_SUITE(ramfs_mmap);

static constexpr size_t PAGE = pmm::PAGE_SIZE;
static constexpr uint32_t PROT_RW = mm::MM_PROT_READ | mm::MM_PROT_WRITE;

// Other tasks and the heap may allocate a few pages while a test runs
static constexpr uint64_t PAGE_COUNT_TOLERANCE = 8;

static uint8_t pattern_byte(size_t i) {
    return static_cast<uint8_t>(i * 13 + 1);
}

static fs::file* create_file_with_pattern(const char* path, size_t len) {
    fs::file* f = fs::open(path, fs::O_CREAT | fs::O_RDWR | fs::O_TRUNC);
    if (!f) {
        return nullptr;
    }

    uint8_t chunk[256];
    for (size_t done = 0; done < len; done += sizeof(chunk)) {
        for (size_t i = 0; i < sizeof(chunk); i++) {
            chunk[i] = pattern_byte(done + i);
        }

        size_t chunk_len = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
        if (fs::write(f, chunk, chunk_len) != static_cast<ssize_t>(chunk_len)) {
            fs::close(f);
            return nullptr;
        }
    }

    return f;
}

static uint8_t* mapped_page_bytes(mm::mm_context* mm_ctx, uintptr_t addr) {
    pmm::phys_addr_t phys = paging::get_physical(addr, mm_ctx->pt_root);
    return phys ? static_cast<uint8_t*>(paging::phys_to_virt(phys)) : nullptr;
}

static bool mapping_matches_pattern(mm::mm_context* mm_ctx, uintptr_t addr, size_t len) {
    for (size_t offset = 0; offset < len; offset += PAGE) {
        uint8_t* page = mapped_page_bytes(mm_ctx, addr + offset);
        if (!page) {
            return false;
        }

        for (size_t i = 0; i < PAGE && offset + i < len; i++) {
            if (page[i] != pattern_byte(offset + i)) {
                return false;
            }
        }
    }

    return true;
}

TEST(ramfs_mmap, mapping_shares_bytes_with_read_and_write) {
    fs::file* f = create_file_with_pattern("/ramfs_mmap_share", 2 * PAGE);
    ASSERT_NOT_NULL(f);

    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(fs::mmap(f, mm_ctx, 0, 2 * PAGE, PROT_RW, mm::MM_MAP_SHARED, 0, &addr),
              mm::MM_CTX_OK);
    EXPECT_TRUE(mapping_matches_pattern(mm_ctx, addr, 2 * PAGE));

    mapped_page_bytes(mm_ctx, addr + PAGE)[7] = 0xAB;
    uint8_t byte = 0;
    ASSERT_EQ(fs::seek(f, static_cast<int64_t>(PAGE + 7), fs::SEEK_SET),
              static_cast<int64_t>(PAGE + 7));
    ASSERT_EQ(fs::read(f, &byte, 1), 1);
    EXPECT_EQ(byte, 0xAB);

    uint8_t value = 0x5C;
    ASSERT_EQ(fs::seek(f, 3, fs::SEEK_SET), 3);
    ASSERT_EQ(fs::write(f, &value, 1), 1);
    EXPECT_EQ(mapped_page_bytes(mm_ctx, addr)[3], 0x5C);

    mm::mm_context_release(mm_ctx);
    fs::close(f);
    fs::unlink("/ramfs_mmap_share");
}

TEST(ramfs_mmap, mapping_outlives_unlink_and_close) {
    constexpr size_t FILE_PAGES = 32;

    page_quarantine::drain();
    uint64_t baseline = pmm::free_page_count();

    fs::file* f = create_file_with_pattern("/ramfs_mmap_orphan", FILE_PAGES * PAGE);
    ASSERT_NOT_NULL(f);

    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(fs::mmap(f, mm_ctx, 0, FILE_PAGES * PAGE, mm::MM_PROT_READ, mm::MM_MAP_SHARED,
                       0, &addr),
              mm::MM_CTX_OK);

    EXPECT_EQ(fs::unlink("/ramfs_mmap_orphan"), fs::OK);
    EXPECT_EQ(fs::close(f), fs::OK);
    EXPECT_TRUE(mapping_matches_pattern(mm_ctx, addr, FILE_PAGES * PAGE));

    mm::mm_context_release(mm_ctx);
    page_quarantine::drain();
    EXPECT_GE(pmm::free_page_count() + PAGE_COUNT_TOLERANCE, baseline);
}

TEST(ramfs_mmap, truncate_keeps_mapped_pages_valid) {
    fs::file* f = create_file_with_pattern("/ramfs_mmap_trunc", 2 * PAGE);
    ASSERT_NOT_NULL(f);

    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(fs::mmap(f, mm_ctx, 0, 2 * PAGE, PROT_RW, mm::MM_MAP_SHARED, 0, &addr),
              mm::MM_CTX_OK);

    fs::node* node = f->get_node();
    ASSERT_EQ(node->truncate(0), fs::OK);
    EXPECT_EQ(node->size(), static_cast<size_t>(0));
    EXPECT_TRUE(mapping_matches_pattern(mm_ctx, addr, 2 * PAGE));

    ASSERT_EQ(node->truncate(2 * PAGE), fs::OK);
    uint8_t byte = 0xFF;
    ASSERT_EQ(fs::seek(f, static_cast<int64_t>(PAGE), fs::SEEK_SET), static_cast<int64_t>(PAGE));
    ASSERT_EQ(fs::read(f, &byte, 1), 1);
    EXPECT_EQ(byte, 0);
    EXPECT_EQ(mapped_page_bytes(mm_ctx, addr + PAGE)[0], 0);

    mm::mm_context_release(mm_ctx);
    fs::close(f);
    fs::unlink("/ramfs_mmap_trunc");
}

TEST(ramfs_mmap, truncating_an_unmapped_file_frees_its_pages) {
    constexpr size_t FILE_PAGES = 32;

    fs::file* f = create_file_with_pattern("/ramfs_mmap_shrink", FILE_PAGES * PAGE);
    ASSERT_NOT_NULL(f);

    page_quarantine::drain();
    uint64_t before = pmm::free_page_count();
    ASSERT_EQ(f->get_node()->truncate(0), fs::OK);

    page_quarantine::drain();
    EXPECT_GE(pmm::free_page_count() + PAGE_COUNT_TOLERANCE, before + FILE_PAGES);

    fs::close(f);
    fs::unlink("/ramfs_mmap_shrink");
}

TEST(ramfs_mmap, mapping_past_end_of_file_fails) {
    fs::file* f = create_file_with_pattern("/ramfs_mmap_short", PAGE);
    ASSERT_NOT_NULL(f);

    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    EXPECT_EQ(fs::mmap(f, mm_ctx, 0, 2 * PAGE, mm::MM_PROT_READ, mm::MM_MAP_SHARED, 0, &addr),
              mm::MM_CTX_ERR_INVALID_ARG);

    mm::mm_context_release(mm_ctx);
    fs::close(f);
    fs::unlink("/ramfs_mmap_short");
}
