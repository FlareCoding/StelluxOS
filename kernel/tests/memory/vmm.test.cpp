#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "boot/boot_services.h"
#include "common/string.h"
#include "common/logging.h"

TEST_SUITE(vmm_test);

namespace {

uint64_t g_initial_free_pages = 0;

int32_t vmm_before_all() {
    g_initial_free_pages = pmm::free_page_count();
    if (g_initial_free_pages < 256) {
        log::error("vmm tests: insufficient free pages (%lu)", g_initial_free_pages);
        return -1;
    }
    return 0;
}

int32_t vmm_after_all() {
    uint64_t final_free = pmm::free_page_count();
    if (final_free != g_initial_free_pages) {
        log::error("vmm tests: leak detected, started=%lu ended=%lu delta=%ld",
                   g_initial_free_pages, final_free,
                   static_cast<int64_t>(final_free) - static_cast<int64_t>(g_initial_free_pages));
    }
    return 0;
}

} // namespace

BEFORE_ALL(vmm_test, vmm_before_all);
AFTER_ALL(vmm_test, vmm_after_all);

TEST(vmm_test, alloc_free_single_page) {
    uintptr_t out = 0;
    int32_t result = vmm::alloc(1, paging::PAGE_KERNEL_RW, 0,
                                kva::tag::generic, out);
    ASSERT_EQ(result, vmm::OK);
    ASSERT_NE(out, static_cast<uintptr_t>(0));
    EXPECT_EQ(vmm::free(out), vmm::OK);
}

TEST(vmm_test, alloc_write_read) {
    uintptr_t out = 0;
    int32_t result = vmm::alloc(1, paging::PAGE_KERNEL_RW, 0,
                                kva::tag::generic, out);
    ASSERT_EQ(result, vmm::OK);

    auto* ptr = reinterpret_cast<volatile uint64_t*>(out);
    *ptr = 0xDEADBEEFULL;
    EXPECT_EQ(*ptr, 0xDEADBEEFULL);

    vmm::free(out);
}

TEST(vmm_test, alloc_zero_flag) {
    uintptr_t out = 0;
    int32_t result = vmm::alloc(1, paging::PAGE_KERNEL_RW, vmm::ALLOC_ZERO,
                                kva::tag::generic, out);
    ASSERT_EQ(result, vmm::OK);

    auto* ptr = reinterpret_cast<volatile uint8_t*>(out);
    bool all_zero = true;
    for (size_t i = 0; i < 64; i++) {
        if (ptr[i] != 0) {
            all_zero = false;
            break;
        }
    }
    EXPECT_TRUE(all_zero);

    vmm::free(out);
}

TEST(vmm_test, alloc_multiple_pages) {
    constexpr size_t N = 4;
    uintptr_t out = 0;
    int32_t result = vmm::alloc(N, paging::PAGE_KERNEL_RW, vmm::ALLOC_ZERO,
                                kva::tag::generic, out);
    ASSERT_EQ(result, vmm::OK);

    for (size_t i = 0; i < N; i++) {
        auto* ptr = reinterpret_cast<volatile uint64_t*>(out + i * paging::PAGE_SIZE_4KB);
        *ptr = 0xBEEF0000ULL + i;
    }
    for (size_t i = 0; i < N; i++) {
        auto* ptr = reinterpret_cast<volatile uint64_t*>(out + i * paging::PAGE_SIZE_4KB);
        EXPECT_EQ(*ptr, 0xBEEF0000ULL + i);
    }

    vmm::free(out);
}

TEST(vmm_test, alloc_contiguous) {
    uintptr_t out_addr = 0;
    pmm::phys_addr_t out_phys = 0;
    int32_t result = vmm::alloc_contiguous(
        4, pmm::ZONE_ANY, paging::PAGE_KERNEL_RW, vmm::ALLOC_ZERO,
        kva::tag::generic, out_addr, out_phys
    );
    ASSERT_EQ(result, vmm::OK);
    ASSERT_NE(out_addr, static_cast<uintptr_t>(0));
    ASSERT_NE(out_phys, static_cast<pmm::phys_addr_t>(0));

    // Physical address must be aligned to the block size (4 pages = 16KB)
    EXPECT_EQ(out_phys & 0x3FFF, static_cast<pmm::phys_addr_t>(0));

    auto* ptr = reinterpret_cast<volatile uint64_t*>(out_addr);
    *ptr = 0xC0FFEEULL;
    EXPECT_EQ(*ptr, 0xC0FFEEULL);

    vmm::free(out_addr);
}

TEST(vmm_test, alloc_contiguous_dma32) {
    uintptr_t out_addr = 0;
    pmm::phys_addr_t out_phys = 0;
    int32_t result = vmm::alloc_contiguous(
        1, pmm::ZONE_DMA32, paging::PAGE_KERNEL_RW, 0,
        kva::tag::generic, out_addr, out_phys
    );
    ASSERT_EQ(result, vmm::OK);
    EXPECT_LT(out_phys, static_cast<pmm::phys_addr_t>(0x100000000ULL));

    vmm::free(out_addr);
}

TEST(vmm_test, alloc_stack_with_guards) {
    uintptr_t out_base = 0;
    uintptr_t out_top = 0;
    int32_t result = vmm::alloc_stack(4, 1, kva::tag::privileged_stack,
                                      out_base, out_top);
    ASSERT_EQ(result, vmm::OK);
    ASSERT_NE(out_base, static_cast<uintptr_t>(0));
    ASSERT_NE(out_top, static_cast<uintptr_t>(0));

    size_t usable_size = out_top - out_base;
    EXPECT_EQ(usable_size, static_cast<size_t>(4 * paging::PAGE_SIZE_4KB));

    vmm::free(out_base);
}

TEST(vmm_test, alloc_stack_usable_region) {
    uintptr_t out_base = 0;
    uintptr_t out_top = 0;
    int32_t result = vmm::alloc_stack(4, 1, kva::tag::privileged_stack,
                                      out_base, out_top);
    ASSERT_EQ(result, vmm::OK);

    // Write near the top of the stack (stacks grow downward)
    auto* ptr = reinterpret_cast<volatile uint64_t*>(out_top - sizeof(uint64_t));
    *ptr = 0x57AC0000ULL;
    EXPECT_EQ(*ptr, 0x57AC0000ULL);

    // Write at the base
    auto* base_ptr = reinterpret_cast<volatile uint64_t*>(out_base);
    *base_ptr = 0xBA5E0000ULL;
    EXPECT_EQ(*base_ptr, 0xBA5E0000ULL);

    vmm::free(out_base);
}

TEST(vmm_test, free_returns_pages_to_pmm) {
    uint64_t before = pmm::free_page_count();

    uintptr_t out = 0;
    int32_t result = vmm::alloc(8, paging::PAGE_KERNEL_RW, 0,
                                kva::tag::generic, out);
    ASSERT_EQ(result, vmm::OK);

    uint64_t during = pmm::free_page_count();
    EXPECT_LT(during, before);

    vmm::free(out);

    uint64_t after = pmm::free_page_count();
    EXPECT_EQ(after, before);
}

TEST(vmm_test, alloc_free_different_order) {
    uintptr_t a = 0, b = 0, c = 0;

    ASSERT_EQ(vmm::alloc(1, paging::PAGE_KERNEL_RW, 0, kva::tag::generic, a), vmm::OK);
    ASSERT_EQ(vmm::alloc(2, paging::PAGE_KERNEL_RW, 0, kva::tag::generic, b), vmm::OK);
    ASSERT_EQ(vmm::alloc(1, paging::PAGE_KERNEL_RW, 0, kva::tag::generic, c), vmm::OK);

    EXPECT_EQ(vmm::free(b), vmm::OK);
    EXPECT_EQ(vmm::free(a), vmm::OK);
    EXPECT_EQ(vmm::free(c), vmm::OK);
}

TEST(vmm_test, map_phys_roundtrip) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    // Write via HHDM
    auto* hhdm_ptr = reinterpret_cast<volatile uint64_t*>(
        reinterpret_cast<uintptr_t>(paging::phys_to_virt(phys)));
    *hhdm_ptr = 0xABCD1234ULL;

    // Map via vmm::map_phys
    uintptr_t out_base = 0;
    uintptr_t out_va = 0;
    int32_t result = vmm::map_phys(phys, paging::PAGE_SIZE_4KB,
                                   paging::PAGE_KERNEL_RW, out_base, out_va);
    ASSERT_EQ(result, vmm::OK);

    // Read via mapped VA
    auto* mapped_ptr = reinterpret_cast<volatile uint64_t*>(out_va);
    EXPECT_EQ(*mapped_ptr, 0xABCD1234ULL);

    vmm::free(out_base);
    pmm::free_page(phys);
}

TEST(vmm_test, stress_alloc_free) {
    constexpr size_t N = 32;
    uintptr_t addrs[N];
    uint64_t before = pmm::free_page_count();

    for (size_t i = 0; i < N; i++) {
        size_t pages = (i % 4) + 1;
        int32_t result = vmm::alloc(pages, paging::PAGE_KERNEL_RW, 0,
                                    kva::tag::generic, addrs[i]);
        ASSERT_EQ(result, vmm::OK);
    }

    for (size_t i = 0; i < N; i++) {
        EXPECT_EQ(vmm::free(addrs[i]), vmm::OK);
    }

    uint64_t after = pmm::free_page_count();
    EXPECT_EQ(after, before);
}
