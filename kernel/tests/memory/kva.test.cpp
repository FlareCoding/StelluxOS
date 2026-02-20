#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "mm/kva.h"
#include "mm/paging_types.h"
#include "common/logging.h"

TEST_SUITE(kva_test);

namespace {

constexpr size_t PAGE_SIZE = paging::PAGE_SIZE_4KB;

int32_t kva_before_all() {
    return 0;
}

int32_t kva_after_all() {
    return 0;
}

} // namespace

BEFORE_ALL(kva_test, kva_before_all);
AFTER_ALL(kva_test, kva_after_all);

TEST(kva_test, alloc_free_basic) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);
    EXPECT_NE(alloc.base, static_cast<uintptr_t>(0));
    EXPECT_EQ(kva::free(alloc.base), kva::OK);
}

TEST(kva_test, alloc_returns_page_aligned) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);
    EXPECT_EQ(alloc.base & (PAGE_SIZE - 1), static_cast<uintptr_t>(0));
    kva::free(alloc.base);
}

TEST(kva_test, alloc_size_rounded_up) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        1, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);
    EXPECT_GE(alloc.size, PAGE_SIZE);
    kva::free(alloc.base);
}

TEST(kva_test, alloc_with_guard_pages) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 1, 1,
        kva::placement::low, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);
    EXPECT_EQ(alloc.guard_pre, static_cast<uint16_t>(1));
    EXPECT_EQ(alloc.guard_post, static_cast<uint16_t>(1));
    EXPECT_GT(alloc.reserved_size, alloc.size);
    kva::free(alloc.base);
}

TEST(kva_test, query_finds_allocation) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);

    kva::allocation queried = {};
    result = kva::query(alloc.base, queried);
    ASSERT_EQ(result, kva::OK);
    EXPECT_EQ(queried.base, alloc.base);
    EXPECT_EQ(queried.size, alloc.size);

    kva::free(alloc.base);
}

TEST(kva_test, query_not_found_after_free) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);
    uintptr_t base = alloc.base;

    kva::free(base);

    kva::allocation queried = {};
    result = kva::query(base, queried);
    EXPECT_EQ(result, kva::ERR_NOT_FOUND);
}

TEST(kva_test, alloc_no_overlap) {
    constexpr size_t N = 10;
    kva::allocation allocs[N];

    for (size_t i = 0; i < N; i++) {
        int32_t result = kva::alloc(
            PAGE_SIZE, PAGE_SIZE, 0, 0,
            kva::placement::low, kva::tag::generic, 0, allocs[i]
        );
        ASSERT_EQ(result, kva::OK);
    }

    for (size_t i = 0; i < N; i++) {
        for (size_t j = i + 1; j < N; j++) {
            uintptr_t a_end = allocs[i].base + allocs[i].size;
            uintptr_t b_end = allocs[j].base + allocs[j].size;
            bool overlaps = allocs[i].base < b_end && allocs[j].base < a_end;
            EXPECT_FALSE(overlaps);
        }
    }

    for (size_t i = 0; i < N; i++) {
        kva::free(allocs[i].base);
    }
}

TEST(kva_test, alloc_placement_low) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);
    EXPECT_NE(alloc.base, static_cast<uintptr_t>(0));
    kva::free(alloc.base);
}

TEST(kva_test, alloc_placement_high) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::high, kva::tag::generic, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);
    EXPECT_NE(alloc.base, static_cast<uintptr_t>(0));
    kva::free(alloc.base);
}

TEST(kva_test, alloc_with_tag) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::privileged_heap, 0, alloc
    );
    ASSERT_EQ(result, kva::OK);

    kva::allocation queried = {};
    int32_t qr = kva::query(alloc.base, queried);
    ASSERT_EQ(qr, kva::OK);
    EXPECT_EQ(static_cast<uint16_t>(queried.alloc_tag),
              static_cast<uint16_t>(kva::tag::privileged_heap));

    kva::free(alloc.base);
}

TEST(kva_test, stress_alloc_free) {
    constexpr size_t N = 64;
    kva::allocation allocs[N];

    for (size_t i = 0; i < N; i++) {
        int32_t result = kva::alloc(
            PAGE_SIZE, PAGE_SIZE, 0, 0,
            kva::placement::low, kva::tag::generic, 0, allocs[i]
        );
        ASSERT_EQ(result, kva::OK);
    }

    // Free in a non-sequential pattern (even first, then odd)
    for (size_t i = 0; i < N; i += 2) {
        kva::free(allocs[i].base);
    }
    for (size_t i = 1; i < N; i += 2) {
        kva::free(allocs[i].base);
    }

    // Alloc again to verify space was recovered
    kva::allocation realloc = {};
    int32_t result = kva::alloc(
        PAGE_SIZE, PAGE_SIZE, 0, 0,
        kva::placement::low, kva::tag::generic, 0, realloc
    );
    ASSERT_EQ(result, kva::OK);
    kva::free(realloc.base);
}
