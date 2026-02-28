#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "sync/mutex.h"

TEST_SUITE(vma_test);

namespace {

uint64_t g_initial_free_pages = 0;

int32_t vma_before_all() {
    g_initial_free_pages = pmm::free_page_count();
    return 0;
}

int32_t vma_after_all() {
    uint64_t final_free = pmm::free_page_count();
    if (final_free != g_initial_free_pages) {
        return -1;
    }
    return 0;
}

constexpr size_t PAGE = pmm::PAGE_SIZE;

} // namespace

BEFORE_ALL(vma_test, vma_before_all);
AFTER_ALL(vma_test, vma_after_all);

TEST(vma_test, create_and_release_context) {
    uint64_t before = pmm::free_page_count();
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    EXPECT_NE(mm_ctx->pt_root, static_cast<pmm::phys_addr_t>(0));

    mm::mm_context_release(mm_ctx);

    uint64_t after = pmm::free_page_count();
    EXPECT_EQ(after, before);
}

TEST(vma_test, insert_vma_rejects_overlap) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t start = mm::MMAP_BASE_DEFAULT + 4 * PAGE;
    ASSERT_EQ(mm::mm_context_add_vma(
        mm_ctx, start, 2 * PAGE,
        mm::MM_PROT_READ, mm::VMA_FLAG_PRIVATE | mm::VMA_FLAG_ELF
    ), mm::MM_CTX_OK);

    EXPECT_EQ(mm::mm_context_add_vma(
        mm_ctx, start + PAGE, 2 * PAGE,
        mm::MM_PROT_READ, mm::VMA_FLAG_PRIVATE | mm::VMA_FLAG_ELF
    ), mm::MM_CTX_ERR_EXISTS);

    mm::mm_context_release(mm_ctx);
}

TEST(vma_test, mmap_and_munmap_roundtrip) {
    uint64_t before = pmm::free_page_count();
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t mapped = 0;
    int32_t rc = mm::mm_context_map_anonymous(
        mm_ctx,
        0,
        3 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS,
        &mapped
    );
    ASSERT_EQ(rc, mm::MM_CTX_OK);
    EXPECT_TRUE(mapped >= mm_ctx->mmap_base);
    EXPECT_TRUE(mapped + 3 * PAGE <= mm_ctx->mmap_end);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), static_cast<size_t>(1));

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, mapped, 3 * PAGE), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), static_cast<size_t>(0));

    mm::mm_context_release(mm_ctx);
    uint64_t after = pmm::free_page_count();
    EXPECT_EQ(after, before);
}

TEST(vma_test, munmap_unmapped_range_is_idempotent) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = mm_ctx->mmap_base + 16 * PAGE;
    EXPECT_EQ(mm::mm_context_unmap(mm_ctx, addr, PAGE), mm::MM_CTX_OK);

    mm::mm_context_release(mm_ctx);
}

TEST(vma_test, map_fixed_noreplace_conflict) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t fixed = mm_ctx->mmap_base + 64 * PAGE;
    uintptr_t out = 0;

    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, fixed, 2 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS | mm::MM_MAP_FIXED,
        &out
    ), mm::MM_CTX_OK);
    EXPECT_EQ(out, fixed);

    EXPECT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, fixed, 2 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS |
            mm::MM_MAP_FIXED_NOREPLACE,
        &out
    ), mm::MM_CTX_ERR_EXISTS);

    mm::mm_context_release(mm_ctx);
}

TEST(vma_test, mprotect_splits_and_merges_vmas) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t mapped = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 4 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS,
        &mapped
    ), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), static_cast<size_t>(1));

    ASSERT_EQ(mm::mm_context_mprotect(
        mm_ctx,
        mapped + PAGE,
        2 * PAGE,
        mm::MM_PROT_READ
    ), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), static_cast<size_t>(3));

    ASSERT_EQ(mm::mm_context_mprotect(
        mm_ctx,
        mapped + PAGE,
        2 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE
    ), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), static_cast<size_t>(1));

    mm::mm_context_release(mm_ctx);
}

TEST(vma_test, prot_none_map_and_mprotect_roundtrip) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t mapped = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx,
        0,
        PAGE,
        0, // PROT_NONE
        mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS,
        &mapped
    ), mm::MM_CTX_OK);

    paging::page_flags_t flags = paging::get_page_flags(mapped, mm_ctx->pt_root);
    EXPECT_TRUE((flags & paging::PAGE_USER) == 0);

    ASSERT_EQ(mm::mm_context_mprotect(
        mm_ctx,
        mapped,
        PAGE,
        mm::MM_PROT_READ
    ), mm::MM_CTX_OK);

    flags = paging::get_page_flags(mapped, mm_ctx->pt_root);
    EXPECT_TRUE((flags & paging::PAGE_USER) != 0);
    EXPECT_TRUE((flags & paging::PAGE_READ) != 0);

    mm::mm_context_release(mm_ctx);
}

TEST(vma_test, gap_search_topdown_prefers_highest_gap) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t first = mm_ctx->mmap_base + 8 * PAGE;
    uintptr_t second = mm_ctx->mmap_base + 32 * PAGE;

    ASSERT_EQ(mm::mm_context_add_vma(
        mm_ctx, first, 4 * PAGE,
        mm::MM_PROT_READ, mm::VMA_FLAG_PRIVATE | mm::VMA_FLAG_ELF
    ), mm::MM_CTX_OK);
    ASSERT_EQ(mm::mm_context_add_vma(
        mm_ctx, second, 4 * PAGE,
        mm::MM_PROT_READ, mm::VMA_FLAG_PRIVATE | mm::VMA_FLAG_ELF
    ), mm::MM_CTX_OK);

    sync::mutex_lock(mm_ctx->lock);
    uintptr_t gap = mm::vma_find_gap_topdown_locked(mm_ctx, 2 * PAGE);
    sync::mutex_unlock(mm_ctx->lock);

    EXPECT_TRUE(gap >= second + 4 * PAGE);
    EXPECT_TRUE(gap + 2 * PAGE <= mm_ctx->mmap_end);

    mm::mm_context_release(mm_ctx);
}
