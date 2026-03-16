#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "dma/dma.h"
#include "mm/pmm.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(dma_test);

namespace {

uint64_t g_initial_free_pages = 0;

int32_t dma_before_all() {
    g_initial_free_pages = pmm::free_page_count();
    if (g_initial_free_pages < 256) {
        log::error("dma tests: insufficient free pages (%lu)", g_initial_free_pages);
        return -1;
    }
    return 0;
}

int32_t dma_after_all() {
    uint64_t final_free = pmm::free_page_count();
    int64_t delta = static_cast<int64_t>(final_free) - static_cast<int64_t>(g_initial_free_pages);
    if (delta < -4) {
        log::error("dma tests: leak detected, started=%lu ended=%lu delta=%ld",
                   g_initial_free_pages, final_free, delta);
    }
    return 0;
}

} // namespace

BEFORE_ALL(dma_test, dma_before_all);
AFTER_ALL(dma_test, dma_after_all);

// ---------- Page-level tests ----------

TEST(dma_test, page_alloc_basic) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(1, buf);
    });
    ASSERT_EQ(rc, dma::OK);
    ASSERT_NE(buf.virt, static_cast<uintptr_t>(0));
    ASSERT_NE(buf.phys, static_cast<pmm::phys_addr_t>(0));
    EXPECT_EQ(buf.size, pmm::PAGE_SIZE);
    EXPECT_ALIGNED(buf.virt, pmm::PAGE_SIZE);
    EXPECT_ALIGNED(buf.phys, pmm::PAGE_SIZE);
    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

TEST(dma_test, page_alloc_multi) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(4, buf);
    });
    ASSERT_EQ(rc, dma::OK);
    ASSERT_NE(buf.virt, static_cast<uintptr_t>(0));
    EXPECT_EQ(buf.size, 4 * pmm::PAGE_SIZE);
    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

TEST(dma_test, page_alloc_free_no_leak) {
    uint64_t before = pmm::free_page_count();
    dma::buffer buf = {};
    RUN_ELEVATED({
        dma::alloc_pages(1, buf);
        dma::free_pages(buf);
    });
    uint64_t after = pmm::free_page_count();
    EXPECT_EQ(before, after);
}

TEST(dma_test, page_alloc_zero_returns_error) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(0, buf);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, page_alloc_dma32_phys_below_4gb) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(1, buf, pmm::ZONE_DMA32);
    });
    ASSERT_EQ(rc, dma::OK);
    EXPECT_LT(buf.phys, static_cast<pmm::phys_addr_t>(0x100000000ULL));
    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

// ---------- Pool init validation tests ----------

TEST(dma_test, pool_init_basic) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(64, 64, 64);
    });
    ASSERT_EQ(rc, dma::OK);
    EXPECT_EQ(p.object_size(), static_cast<size_t>(64));
    EXPECT_EQ(p.capacity(), static_cast<size_t>(64));
    EXPECT_EQ(p.used_count(), static_cast<size_t>(0));
    RUN_ELEVATED({
        p.destroy();
    });
}

TEST(dma_test, pool_init_zero_size) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(0, 64, 64);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, pool_init_zero_capacity) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(64, 64, 0);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, pool_init_oversize) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(pmm::PAGE_SIZE + 1, 64, 1);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, pool_init_bad_alignment) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(64, 3, 64);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

// ---------- Pool alloc/free tests ----------

TEST(dma_test, pool_alloc_returns_aligned) {
    dma::pool p = {};
    dma::buffer buf = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(64, 64, 4), dma::OK);
        ASSERT_EQ(p.alloc(buf), dma::OK);
    });
    EXPECT_ALIGNED(buf.virt, 64);
    EXPECT_ALIGNED(buf.phys, 64);
    RUN_ELEVATED({
        p.free(buf);
        p.destroy();
    });
}

TEST(dma_test, pool_alloc_returns_phys) {
    dma::pool p = {};
    dma::buffer buf = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(128, 64, 4), dma::OK);
        ASSERT_EQ(p.alloc(buf), dma::OK);
    });
    ASSERT_NE(buf.phys, static_cast<pmm::phys_addr_t>(0));
    EXPECT_EQ(buf.size, static_cast<size_t>(128));
    RUN_ELEVATED({
        p.free(buf);
        p.destroy();
    });
}

TEST(dma_test, pool_alloc_distinct_phys) {
    dma::pool p = {};
    constexpr size_t N = 4;
    dma::buffer bufs[N] = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(256, 64, N), dma::OK);
        for (size_t i = 0; i < N; i++) {
            ASSERT_EQ(p.alloc(bufs[i]), dma::OK);
        }
    });
    for (size_t i = 0; i < N; i++) {
        for (size_t j = i + 1; j < N; j++) {
            EXPECT_NE(bufs[i].phys, bufs[j].phys);
            EXPECT_NE(bufs[i].virt, bufs[j].virt);
        }
    }
    RUN_ELEVATED({
        for (size_t i = 0; i < N; i++) {
            p.free(bufs[i]);
        }
        p.destroy();
    });
}

TEST(dma_test, pool_alloc_free_cycle) {
    dma::pool p = {};
    constexpr size_t CAP = 8;
    dma::buffer bufs[CAP] = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(64, 64, CAP), dma::OK);

        for (size_t i = 0; i < CAP; i++) {
            ASSERT_EQ(p.alloc(bufs[i]), dma::OK);
        }
        EXPECT_EQ(p.used_count(), CAP);

        for (size_t i = 0; i < CAP; i++) {
            p.free(bufs[i]);
        }
        EXPECT_EQ(p.used_count(), static_cast<size_t>(0));

        for (size_t i = 0; i < CAP; i++) {
            ASSERT_EQ(p.alloc(bufs[i]), dma::OK);
        }
        EXPECT_EQ(p.used_count(), CAP);

        for (size_t i = 0; i < CAP; i++) {
            p.free(bufs[i]);
        }
        p.destroy();
    });
}

TEST(dma_test, pool_alloc_exhaustion) {
    dma::pool p = {};
    constexpr size_t CAP = 4;
    dma::buffer bufs[CAP] = {};
    dma::buffer extra = {};
    int32_t extra_rc = 0;
    RUN_ELEVATED({
        ASSERT_EQ(p.init(64, 64, CAP), dma::OK);
        for (size_t i = 0; i < CAP; i++) {
            ASSERT_EQ(p.alloc(bufs[i]), dma::OK);
        }
        extra_rc = p.alloc(extra);
    });
    EXPECT_EQ(extra_rc, dma::ERR_FULL);
    EXPECT_EQ(p.used_count(), CAP);
    RUN_ELEVATED({
        for (size_t i = 0; i < CAP; i++) {
            p.free(bufs[i]);
        }
        p.destroy();
    });
}

// ---------- Pool lifecycle tests ----------

TEST(dma_test, pool_destroy_and_reinit) {
    dma::pool p = {};
    dma::buffer buf = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(64, 64, 4), dma::OK);
        ASSERT_EQ(p.alloc(buf), dma::OK);
        p.free(buf);
        p.destroy();

        ASSERT_EQ(p.init(128, 64, 8), dma::OK);
        ASSERT_EQ(p.alloc(buf), dma::OK);
        EXPECT_EQ(buf.size, static_cast<size_t>(128));
        p.free(buf);
        p.destroy();
    });
}

TEST(dma_test, pool_last_slab_capacity) {
    dma::pool p = {};
    constexpr size_t CAP = 5; // objs_per_page=4 (PAGE_SIZE/1024), so last slab has 1 obj
    dma::buffer bufs[CAP] = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(1024, 64, CAP);
        ASSERT_EQ(rc, dma::OK);
        for (size_t i = 0; i < CAP; i++) {
            ASSERT_EQ(p.alloc(bufs[i]), dma::OK);
        }
    });
    EXPECT_EQ(p.used_count(), CAP);

    dma::buffer extra = {};
    RUN_ELEVATED({
        rc = p.alloc(extra);
    });
    EXPECT_EQ(rc, dma::ERR_FULL);

    RUN_ELEVATED({
        for (size_t i = 0; i < CAP; i++) {
            p.free(bufs[i]);
        }
        p.destroy();
    });
}

TEST(dma_test, pool_1024_byte_objects) {
    dma::pool p = {};
    constexpr size_t CAP = 8;
    dma::buffer bufs[CAP] = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(1024, 64, CAP), dma::OK);
        for (size_t i = 0; i < CAP; i++) {
            ASSERT_EQ(p.alloc(bufs[i]), dma::OK);
            EXPECT_ALIGNED(bufs[i].virt, 64);
            EXPECT_ALIGNED(bufs[i].phys, 64);
            EXPECT_EQ(bufs[i].size, static_cast<size_t>(1024));
        }
        for (size_t i = 0; i < CAP; i++) {
            p.free(bufs[i]);
        }
        p.destroy();
    });
}
