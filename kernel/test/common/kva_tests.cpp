#include "test/framework/test_framework.h"
#include "mm/kva.h"
#include "mm/pmm.h"

STLX_TEST_SUITE(mm_kva, test::phase::post_mm);

STLX_TEST(mm_kva, alloc_query_free_roundtrip_with_guards) {
    kva::allocation alloc = {};
    kva::allocation query = {};
    bool allocated = false;

    if (!STLX_EXPECT_EQ(ctx,
                        kva::alloc(2 * pmm::PAGE_SIZE,
                                   pmm::PAGE_SIZE,
                                   1,
                                   1,
                                   kva::placement::low,
                                   kva::tag::boot,
                                   0,
                                   alloc),
                        kva::OK)) {
        goto cleanup;
    }
    allocated = true;

    STLX_EXPECT_EQ(ctx, alloc.size, static_cast<size_t>(2 * pmm::PAGE_SIZE));
    STLX_EXPECT_EQ(ctx, alloc.base, alloc.reserved_base + pmm::PAGE_SIZE);
    STLX_EXPECT_EQ(ctx, alloc.guard_pre, static_cast<uint16_t>(1));
    STLX_EXPECT_EQ(ctx, alloc.guard_post, static_cast<uint16_t>(1));

    STLX_EXPECT_EQ(ctx, kva::query(alloc.base, query), kva::OK);
    STLX_EXPECT_EQ(ctx, query.base, alloc.base);
    STLX_EXPECT_EQ(ctx, query.alloc_tag, kva::tag::boot);

    // Query from inside pre-guard must still resolve to this allocation.
    STLX_EXPECT_EQ(ctx, kva::query(alloc.reserved_base, query), kva::OK);
    STLX_EXPECT_EQ(ctx, query.base, alloc.base);

    STLX_EXPECT_EQ(ctx, kva::free(alloc.base), kva::OK);
    allocated = false;

    STLX_EXPECT_EQ(ctx, kva::query(alloc.base, query), kva::ERR_NOT_FOUND);

cleanup:
    if (allocated) {
        kva::free(alloc.base);
    }
}

STLX_TEST(mm_kva, invalid_alignment_is_rejected) {
    kva::allocation alloc = {};
    STLX_ASSERT_EQ(ctx,
                   kva::alloc(pmm::PAGE_SIZE,
                              123,
                              0,
                              0,
                              kva::placement::low,
                              kva::tag::generic,
                              0,
                              alloc),
                   kva::ERR_ALIGNMENT);
}

STLX_TEST(mm_kva, reserve_then_free_range) {
    kva::allocation alloc = {};
    kva::allocation query = {};
    bool reserved = false;

    if (!STLX_EXPECT_EQ(ctx,
                        kva::alloc(pmm::PAGE_SIZE,
                                   pmm::PAGE_SIZE,
                                   0,
                                   0,
                                   kva::placement::low,
                                   kva::tag::generic,
                                   0,
                                   alloc),
                        kva::OK)) {
        goto cleanup;
    }

    if (!STLX_EXPECT_EQ(ctx, kva::free(alloc.base), kva::OK)) {
        goto cleanup;
    }

    if (!STLX_EXPECT_EQ(ctx, kva::reserve(alloc.base, pmm::PAGE_SIZE, kva::tag::boot), kva::OK)) {
        goto cleanup;
    }
    reserved = true;

    STLX_EXPECT_EQ(ctx, kva::query(alloc.base, query), kva::OK);
    STLX_EXPECT_EQ(ctx, query.alloc_tag, kva::tag::boot);

cleanup:
    if (reserved) {
        kva::free(alloc.base);
    }
}

STLX_TEST(mm_kva, high_placement_returns_higher_addresses_than_low) {
    kva::allocation low = {};
    kva::allocation high = {};
    bool low_alloc = false;
    bool high_alloc = false;

    if (!STLX_EXPECT_EQ(ctx,
                        kva::alloc(pmm::PAGE_SIZE,
                                   pmm::PAGE_SIZE,
                                   0,
                                   0,
                                   kva::placement::low,
                                   kva::tag::generic,
                                   0,
                                   low),
                        kva::OK)) {
        goto cleanup;
    }
    low_alloc = true;

    if (!STLX_EXPECT_EQ(ctx,
                        kva::alloc(pmm::PAGE_SIZE,
                                   pmm::PAGE_SIZE,
                                   0,
                                   0,
                                   kva::placement::high,
                                   kva::tag::generic,
                                   0,
                                   high),
                        kva::OK)) {
        goto cleanup;
    }
    high_alloc = true;

    STLX_EXPECT_TRUE(ctx, high.base > low.base);

cleanup:
    if (high_alloc) {
        kva::free(high.base);
    }
    if (low_alloc) {
        kva::free(low.base);
    }
}
