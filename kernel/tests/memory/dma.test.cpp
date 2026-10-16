#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "dma/dma.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/page_quarantine.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace dma {

struct device_profile_test_access {
    static device_profile build(uint8_t address_bits, bool coherent) {
        return device_profile(address_bits, coherent);
    }
};

} // namespace dma

TEST_SUITE(dma_test);

constexpr uint8_t ALL_MEMORY_BITS = 64;
constexpr uint8_t DMA32_BITS = 32;
constexpr pmm::phys_addr_t FOUR_GIB = 0x100000000ULL;

static uint64_t g_initial_free_pages = 0;

static int32_t dma_before_all() {
    page_quarantine::drain();
    g_initial_free_pages = pmm::free_page_count();
    if (g_initial_free_pages < 256) {
        log::error("dma tests: insufficient free pages (%lu)", g_initial_free_pages);
        return -1;
    }

    return 0;
}

static int32_t dma_after_all() {
    uint64_t final_free = pmm::free_page_count();
    int64_t delta = static_cast<int64_t>(final_free) - static_cast<int64_t>(g_initial_free_pages);
    if (delta < -4) {
        log::error("dma tests: leak detected, started=%lu ended=%lu delta=%ld",
                   g_initial_free_pages, final_free, delta);
    }
    return 0;
}

BEFORE_ALL(dma_test, dma_before_all);
AFTER_ALL(dma_test, dma_after_all);

// Placement does not matter to these tests, so the device reaches all memory
static dma::device_profile full_range_device() {
    return dma::uncached_profile(ALL_MEMORY_BITS);
}

// Page-level tests

TEST(dma_test, page_alloc_basic) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(full_range_device(), 1, dma::caching::uncached, 0, buf);
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
        rc = dma::alloc_pages(full_range_device(), 4, dma::caching::uncached, 0, buf);
    });
    ASSERT_EQ(rc, dma::OK);
    ASSERT_NE(buf.virt, static_cast<uintptr_t>(0));
    EXPECT_EQ(buf.size, 4 * pmm::PAGE_SIZE);
    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

TEST(dma_test, page_alloc_free_no_leak) {
    page_quarantine::drain();
    uint64_t before = pmm::free_page_count();
    dma::buffer buf = {};
    RUN_ELEVATED({
        (void)dma::alloc_pages(full_range_device(), 1, dma::caching::uncached, 0, buf);
        dma::free_pages(buf);
        page_quarantine::drain();
    });
    uint64_t after = pmm::free_page_count();
    EXPECT_EQ(before, after);
}

TEST(dma_test, page_alloc_zero_returns_error) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(full_range_device(), 0, dma::caching::uncached, 0, buf);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

// One page past the largest block fails whatever memory is free, leaving `out` empty
TEST(dma_test, page_alloc_more_pages_than_the_largest_block_returns_no_mem) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(
            full_range_device(),
            pmm::order_to_pages(pmm::MAX_ORDER) + 1,
            dma::caching::uncached,
            0,
            buf
        );
    });

    EXPECT_EQ(rc, dma::ERR_NO_MEM);
    EXPECT_EQ(buf.virt, static_cast<uintptr_t>(0));
    EXPECT_EQ(buf.size, static_cast<size_t>(0));

    if (rc == dma::OK) {
        RUN_ELEVATED({
            dma::free_pages(buf);
        });
    }
}

TEST(dma_test, page_alloc_dma32_phys_below_4gb) {
    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(dma::uncached_profile(DMA32_BITS), 1, dma::caching::uncached, 0, buf);
    });
    ASSERT_EQ(rc, dma::OK);
    EXPECT_LE(buf.phys + buf.size, FOUR_GIB);
    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

// The test machine's memory ends below 8 GiB, so a 33-bit limit always fits
TEST(dma_test, page_alloc_33_bit_phys_below_8gb) {
    constexpr uint8_t BITS = 33;
    constexpr pmm::phys_addr_t LIMIT = static_cast<pmm::phys_addr_t>(1) << BITS;
    dma::buffer buf = {};
    int32_t rc = 0;

    RUN_ELEVATED({
        rc = dma::alloc_pages(dma::uncached_profile(BITS), 4, dma::caching::uncached, 0, buf);
    });

    ASSERT_EQ(rc, dma::OK);
    EXPECT_LE(buf.phys + buf.size, LIMIT);

    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

TEST(dma_test, page_alloc_limit_outside_range_returns_error) {
    dma::buffer narrow = {};
    dma::buffer wide = {};
    int32_t narrow_rc = 0;
    int32_t wide_rc = 0;

    RUN_ELEVATED({
        narrow_rc = dma::alloc_pages(dma::uncached_profile(DMA32_BITS - 1), 1, dma::caching::uncached, 0, narrow);
        wide_rc = dma::alloc_pages(dma::uncached_profile(ALL_MEMORY_BITS + 1), 1, dma::caching::uncached, 0, wide);
    });

    EXPECT_EQ(narrow_rc, dma::ERR_INVALID_ARG);
    EXPECT_EQ(wide_rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, page_alloc_coherent_device_gets_cached_memory) {
    dma::device_profile device = dma::device_profile_test_access::build(ALL_MEMORY_BITS, true);
    dma::buffer buf = {};
    int32_t rc = 0;
    paging::page_flags_t flags = 0;

    RUN_ELEVATED({
        rc = dma::alloc_pages(device, 1, dma::caching::cached_when_coherent, 0, buf);
        flags = paging::get_page_flags(buf.virt, paging::get_kernel_pt_root());
    });

    ASSERT_EQ(rc, dma::OK);
    EXPECT_TRUE(buf.cached);
    EXPECT_EQ(flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);

    RUN_ELEVATED({
        dma::free_pages(buf);
    });

    EXPECT_FALSE(buf.cached);
}

TEST(dma_test, page_alloc_non_coherent_device_gets_uncached_memory) {
    dma::device_profile device = dma::device_profile_test_access::build(ALL_MEMORY_BITS, false);
    dma::buffer buf = {};
    int32_t rc = 0;
    paging::page_flags_t flags = 0;

    RUN_ELEVATED({
        rc = dma::alloc_pages(device, 1, dma::caching::cached_when_coherent, 0, buf);
        flags = paging::get_page_flags(buf.virt, paging::get_kernel_pt_root());
    });

    ASSERT_EQ(rc, dma::OK);
    EXPECT_FALSE(buf.cached);
    EXPECT_NE(flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);

    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

TEST(dma_test, page_alloc_uncached_mode_ignores_coherence) {
    dma::device_profile device = dma::device_profile_test_access::build(ALL_MEMORY_BITS, true);
    dma::buffer buf = {};
    int32_t rc = 0;
    paging::page_flags_t flags = 0;

    RUN_ELEVATED({
        rc = dma::alloc_pages(device, 1, dma::caching::uncached, 0, buf);
        flags = paging::get_page_flags(buf.virt, paging::get_kernel_pt_root());
    });

    ASSERT_EQ(rc, dma::OK);
    EXPECT_FALSE(buf.cached);
    EXPECT_NE(flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);

    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

TEST(dma_test, uncached_profile_is_not_coherent) {
    dma::device_profile device = dma::uncached_profile(ALL_MEMORY_BITS);
    EXPECT_EQ(device.address_bits(), ALL_MEMORY_BITS);
    EXPECT_FALSE(device.coherent());
}

TEST(dma_test, page_alloc_user_access_maps_user) {
    dma::buffer kernel_buf = {};
    dma::buffer user_buf = {};
    int32_t kernel_rc = 0;
    int32_t user_rc = 0;
    paging::page_flags_t kernel_flags = 0;
    paging::page_flags_t user_flags = 0;

    RUN_ELEVATED({
        kernel_rc = dma::alloc_pages(full_range_device(), 1, dma::caching::uncached, 0, kernel_buf);
        user_rc = dma::alloc_pages(full_range_device(), 1, dma::caching::uncached, paging::PAGE_USER, user_buf);
        kernel_flags = paging::get_page_flags(kernel_buf.virt, paging::get_kernel_pt_root());
        user_flags = paging::get_page_flags(user_buf.virt, paging::get_kernel_pt_root());
    });

    EXPECT_EQ(kernel_rc, dma::OK);
    EXPECT_EQ(user_rc, dma::OK);
    EXPECT_BITS_CLEAR(kernel_flags, paging::PAGE_USER);
    EXPECT_BITS_SET(user_flags, paging::PAGE_USER);

    RUN_ELEVATED({
        dma::free_pages(kernel_buf);
        dma::free_pages(user_buf);
    });
}

// Only `PAGE_USER` may be added, since caching comes from the mode and DMA memory is never executable
TEST(dma_test, page_alloc_access_other_than_user_returns_error) {
    dma::buffer exec_buf = {};
    dma::buffer wc_buf = {};
    int32_t exec_rc = 0;
    int32_t wc_rc = 0;

    RUN_ELEVATED({
        exec_rc = dma::alloc_pages(full_range_device(), 1, dma::caching::uncached, paging::PAGE_EXEC, exec_buf);
        wc_rc = dma::alloc_pages(full_range_device(), 1, dma::caching::uncached, paging::PAGE_WC, wc_buf);
    });

    EXPECT_EQ(exec_rc, dma::ERR_INVALID_ARG);
    EXPECT_EQ(wc_rc, dma::ERR_INVALID_ARG);
}

// Pool init validation tests

TEST(dma_test, pool_init_basic) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(full_range_device(), 64, 64, 64);
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
        rc = p.init(full_range_device(), 0, 64, 64);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, pool_init_zero_capacity) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(full_range_device(), 64, 64, 0);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, pool_init_oversize) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(full_range_device(), pmm::PAGE_SIZE + 1, 64, 1);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, pool_init_bad_alignment) {
    dma::pool p = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = p.init(full_range_device(), 64, 3, 64);
    });
    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

TEST(dma_test, pool_init_narrow_limit) {
    dma::pool p = {};
    int32_t rc = 0;

    RUN_ELEVATED({
        rc = p.init(dma::uncached_profile(DMA32_BITS - 1), 64, 64, 4);
    });

    EXPECT_EQ(rc, dma::ERR_INVALID_ARG);
}

// Pool alloc/free tests

TEST(dma_test, pool_alloc_dma32_uncached_below_4gb) {
    dma::pool p = {};
    dma::buffer buf = {};
    int32_t init_rc = 0;
    int32_t alloc_rc = 0;

    // A reused buffer may still have `cached` set, so `pool::alloc` must clear the flag
    buf.cached = true;

    RUN_ELEVATED({
        init_rc = p.init(dma::uncached_profile(DMA32_BITS), 64, 64, 4);
        alloc_rc = p.alloc(buf);
    });

    ASSERT_EQ(init_rc, dma::OK);
    ASSERT_EQ(alloc_rc, dma::OK);
    EXPECT_LE(buf.phys + buf.size, FOUR_GIB);
    EXPECT_FALSE(buf.cached);

    RUN_ELEVATED({
        p.free(buf);
        p.destroy();
    });
}

TEST(dma_test, pool_alloc_returns_aligned) {
    dma::pool p = {};
    dma::buffer buf = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(full_range_device(), 64, 64, 4), dma::OK);
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
        ASSERT_EQ(p.init(full_range_device(), 128, 64, 4), dma::OK);
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
        ASSERT_EQ(p.init(full_range_device(), 256, 64, N), dma::OK);
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
        ASSERT_EQ(p.init(full_range_device(), 64, 64, CAP), dma::OK);

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
        ASSERT_EQ(p.init(full_range_device(), 64, 64, CAP), dma::OK);
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

// Pool lifecycle tests

TEST(dma_test, pool_destroy_and_reinit) {
    dma::pool p = {};
    dma::buffer buf = {};
    RUN_ELEVATED({
        ASSERT_EQ(p.init(full_range_device(), 64, 64, 4), dma::OK);
        ASSERT_EQ(p.alloc(buf), dma::OK);
        p.free(buf);
        p.destroy();

        ASSERT_EQ(p.init(full_range_device(), 128, 64, 8), dma::OK);
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
        rc = p.init(full_range_device(), 1024, 64, CAP);
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
        ASSERT_EQ(p.init(full_range_device(), 1024, 64, CAP), dma::OK);
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
