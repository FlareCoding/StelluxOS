#include "test/framework/test_framework.h"
#include "mm/pmm.h"

STLX_TEST_SUITE(mm_pmm, test::phase::post_mm);

STLX_TEST(mm_pmm, alloc_free_single_page_preserves_free_count) {
    uint64_t before = pmm::free_page_count();
    pmm::phys_addr_t page = pmm::alloc_page();

    STLX_ASSERT_NE(ctx, page, static_cast<pmm::phys_addr_t>(0));
    STLX_ASSERT_EQ(ctx, pmm::free_page_count(), before - 1);

    STLX_ASSERT_EQ(ctx, pmm::free_page(page), pmm::OK);
    STLX_ASSERT_EQ(ctx, pmm::free_page_count(), before);
}

STLX_TEST(mm_pmm, alloc_free_contiguous_block_honors_alignment) {
    constexpr uint8_t ORDER = 4; // 16 pages

    pmm::phys_addr_t block = pmm::alloc_pages(ORDER, pmm::ZONE_ANY);
    STLX_ASSERT_NE(ctx, block, static_cast<pmm::phys_addr_t>(0));
    STLX_ASSERT_EQ(ctx, block & (pmm::order_to_bytes(ORDER) - 1), static_cast<uint64_t>(0));
    STLX_ASSERT_EQ(ctx, pmm::free_pages(block, ORDER), pmm::OK);
}

STLX_TEST(mm_pmm, invalid_unaligned_free_is_rejected) {
    STLX_ASSERT_EQ(ctx, pmm::free_pages(123, 0), pmm::ERR_INVALID_ADDR);
}

STLX_TEST(mm_pmm, dma32_allocation_stays_below_4gb) {
    constexpr uint64_t DMA32_LIMIT = 0x100000000ULL;

    pmm::phys_addr_t page = pmm::alloc_page(pmm::ZONE_DMA32);
    STLX_ASSERT_NE(ctx, page, static_cast<pmm::phys_addr_t>(0));
    STLX_ASSERT_TRUE(ctx, page < DMA32_LIMIT);
    STLX_ASSERT_EQ(ctx, pmm::free_page(page), pmm::OK);
}

STLX_TEST(mm_pmm, double_free_is_detected) {
    pmm::phys_addr_t page = pmm::alloc_page();
    STLX_ASSERT_NE(ctx, page, static_cast<pmm::phys_addr_t>(0));

    STLX_ASSERT_EQ(ctx, pmm::free_page(page), pmm::OK);
    STLX_ASSERT_EQ(ctx, pmm::free_page(page), pmm::ERR_DOUBLE_FREE);
}
