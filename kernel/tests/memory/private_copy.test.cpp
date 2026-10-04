#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/page_quarantine.h"

TEST_SUITE(private_copy);

static constexpr size_t PAGE = pmm::PAGE_SIZE;
static constexpr uint64_t NEVER_FAIL = ~0ULL;

struct numbered_source {
    uint64_t size;    // bytes the source holds
    uint64_t fail_at; // offset in the mapping whose fill fails
};

static uint64_t g_initial_free_pages = 0;

static int32_t private_copy_before_all() {
    page_quarantine::drain();
    g_initial_free_pages = pmm::free_page_count();
    return 0;
}

static int32_t private_copy_after_all() {
    page_quarantine::drain();
    return pmm::free_page_count() == g_initial_free_pages ? 0 : -1;
}

BEFORE_ALL(private_copy, private_copy_before_all);
AFTER_ALL(private_copy, private_copy_after_all);

// Writes each page's one-based index in the mapping into its first byte
static int64_t fill_numbered_page(void* source, uint64_t offset, uint8_t* page) {
    auto* numbered = static_cast<numbered_source*>(source);
    if (offset == numbered->fail_at) {
        return mm::MM_CTX_ERR_MAP_FAILED;
    }

    if (offset >= numbered->size) {
        return 0;
    }

    page[0] = static_cast<uint8_t>(offset / PAGE + 1);
    return static_cast<int64_t>(PAGE);
}

static uint8_t* mapped_page_bytes(mm::mm_context* mm_ctx, uintptr_t addr) {
    pmm::phys_addr_t phys = paging::get_physical(addr, mm_ctx->pt_root);
    return phys ? static_cast<uint8_t*>(paging::phys_to_virt(phys)) : nullptr;
}

TEST(private_copy, every_page_is_filled_before_the_mapping_appears) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    numbered_source source = {3 * PAGE, NEVER_FAIL};
    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_private_copy(mm_ctx, 0, 3 * PAGE, mm::MM_PROT_READ, mm::MM_MAP_PRIVATE,
                                              fill_numbered_page, &source, &addr), mm::MM_CTX_OK);

    for (size_t i = 0; i < 3; i++) {
        ASSERT_NOT_NULL(mapped_page_bytes(mm_ctx, addr + i * PAGE));
        EXPECT_EQ(mapped_page_bytes(mm_ctx, addr + i * PAGE)[0], static_cast<uint8_t>(i + 1));
    }

    EXPECT_BITS_CLEAR(paging::get_page_flags(addr, mm_ctx->pt_root), paging::PAGE_WRITE);

    mm::mm_context_release(mm_ctx);
}

TEST(private_copy, pages_past_the_end_of_the_source_stay_unmapped) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    numbered_source source = {PAGE, NEVER_FAIL};
    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_private_copy(mm_ctx, 0, 3 * PAGE, mm::MM_PROT_READ, mm::MM_MAP_PRIVATE,
                                              fill_numbered_page, &source, &addr), mm::MM_CTX_OK);

    ASSERT_NOT_NULL(mapped_page_bytes(mm_ctx, addr));
    EXPECT_EQ(mapped_page_bytes(mm_ctx, addr)[0], static_cast<uint8_t>(1));
    EXPECT_NULL(mapped_page_bytes(mm_ctx, addr + PAGE));
    EXPECT_EQ(mm::handle_user_pf(mm_ctx, addr + 2 * PAGE, 0), mm::MM_CTX_ERR_NO_BACKING);

    mm::mm_context_release(mm_ctx);
}

TEST(private_copy, a_failed_fill_maps_nothing) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    numbered_source source = {3 * PAGE, PAGE};
    uintptr_t addr = 0;
    EXPECT_EQ(mm::mm_context_map_private_copy(mm_ctx, 0, 3 * PAGE, mm::MM_PROT_READ, mm::MM_MAP_PRIVATE,
                                              fill_numbered_page, &source, &addr), mm::MM_CTX_ERR_MAP_FAILED);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), static_cast<size_t>(0));

    mm::mm_context_release(mm_ctx);
}

TEST(private_copy, discard_keeps_the_copied_pages) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    numbered_source source = {PAGE, NEVER_FAIL};
    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_private_copy(mm_ctx, 0, PAGE, mm::MM_PROT_READ | mm::MM_PROT_WRITE,
                                              mm::MM_MAP_PRIVATE, fill_numbered_page, &source, &addr),
              mm::MM_CTX_OK);

    EXPECT_EQ(mm::mm_context_discard(mm_ctx, addr, PAGE), mm::MM_CTX_OK);
    ASSERT_NOT_NULL(mapped_page_bytes(mm_ctx, addr));
    EXPECT_EQ(mapped_page_bytes(mm_ctx, addr)[0], static_cast<uint8_t>(1));

    mm::mm_context_release(mm_ctx);
}

TEST(private_copy, the_mapping_can_shrink_but_not_grow) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    numbered_source source = {2 * PAGE, NEVER_FAIL};
    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_private_copy(mm_ctx, 0, 2 * PAGE, mm::MM_PROT_READ, mm::MM_MAP_PRIVATE,
                                              fill_numbered_page, &source, &addr), mm::MM_CTX_OK);

    uintptr_t result = 0;
    EXPECT_EQ(mm::mm_context_remap(mm_ctx, addr, 2 * PAGE, 3 * PAGE, mm::MM_REMAP_MAYMOVE, 0, &result),
              mm::MM_CTX_ERR_CANNOT_GROW);
    EXPECT_EQ(mm::mm_context_remap(mm_ctx, addr, 2 * PAGE, PAGE, 0, 0, &result), mm::MM_CTX_OK);
    EXPECT_NOT_NULL(mapped_page_bytes(mm_ctx, addr));
    EXPECT_NULL(mapped_page_bytes(mm_ctx, addr + PAGE));

    mm::mm_context_release(mm_ctx);
}
