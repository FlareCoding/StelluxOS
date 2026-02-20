#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/kva.h"
#include "boot/boot_services.h"
#include "common/string.h"
#include "common/logging.h"

TEST_SUITE(paging_test);

namespace {

uint64_t g_initial_free_pages = 0;

int32_t paging_before_all() {
    g_initial_free_pages = pmm::free_page_count();
    return 0;
}

int32_t paging_after_all() {
    uint64_t final_free = pmm::free_page_count();
    if (final_free != g_initial_free_pages) {
        log::error("paging tests: leak detected, started=%lu ended=%lu",
                   g_initial_free_pages, final_free);
    }
    return 0;
}

// Helper: allocate a VA from KVA for test mappings
paging::virt_addr_t alloc_test_va(size_t pages = 1) {
    kva::allocation alloc = {};
    int32_t result = kva::alloc(
        pages * paging::PAGE_SIZE_4KB,
        paging::PAGE_SIZE_4KB,
        0, 0,
        kva::placement::low,
        kva::tag::generic,
        0,
        alloc
    );
    if (result != kva::OK) return 0;
    return static_cast<paging::virt_addr_t>(alloc.base);
}

void free_test_va(paging::virt_addr_t va) {
    kva::free(static_cast<uintptr_t>(va));
}

} // namespace

BEFORE_ALL(paging_test, paging_before_all);
AFTER_ALL(paging_test, paging_after_all);

TEST(paging_test, map_unmap_single_page) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    ASSERT_EQ(paging::map_page(va, phys, paging::PAGE_KERNEL_RW, root), paging::OK);

    // Write and read back
    auto* ptr = reinterpret_cast<volatile uint64_t*>(va);
    *ptr = 0xDEADBEEFCAFEBABEULL;
    EXPECT_EQ(*ptr, 0xDEADBEEFCAFEBABEULL);

    ASSERT_EQ(paging::unmap_page(va, root), paging::OK);
    paging::flush_tlb_page(va);
    free_test_va(va);
    pmm::free_page(phys);
}

TEST(paging_test, is_mapped_after_map) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    EXPECT_FALSE(paging::is_mapped(va, root));
    ASSERT_EQ(paging::map_page(va, phys, paging::PAGE_KERNEL_RW, root), paging::OK);
    EXPECT_TRUE(paging::is_mapped(va, root));

    paging::unmap_page(va, root);
    paging::flush_tlb_page(va);
    EXPECT_FALSE(paging::is_mapped(va, root));

    free_test_va(va);
    pmm::free_page(phys);
}

TEST(paging_test, get_physical_roundtrip) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    ASSERT_EQ(paging::map_page(va, phys, paging::PAGE_KERNEL_RW, root), paging::OK);

    pmm::phys_addr_t got = paging::get_physical(va, root);
    EXPECT_EQ(got, phys);

    paging::unmap_page(va, root);
    paging::flush_tlb_page(va);
    free_test_va(va);
    pmm::free_page(phys);
}

TEST(paging_test, get_page_flags_match) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    ASSERT_EQ(paging::map_page(va, phys, paging::PAGE_KERNEL_RW, root), paging::OK);

    paging::page_flags_t flags = paging::get_page_flags(va, root);
    EXPECT_TRUE((flags & paging::PAGE_READ) != 0);
    EXPECT_TRUE((flags & paging::PAGE_WRITE) != 0);

    paging::unmap_page(va, root);
    paging::flush_tlb_page(va);
    free_test_va(va);
    pmm::free_page(phys);
}

TEST(paging_test, set_page_flags) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    ASSERT_EQ(paging::map_page(va, phys, paging::PAGE_KERNEL_RW, root), paging::OK);

    ASSERT_EQ(paging::set_page_flags(va, paging::PAGE_KERNEL_RO, root), paging::OK);
    paging::flush_tlb_page(va);

    paging::page_flags_t flags = paging::get_page_flags(va, root);
    EXPECT_TRUE((flags & paging::PAGE_READ) != 0);
    EXPECT_FALSE((flags & paging::PAGE_WRITE) != 0);

    paging::unmap_page(va, root);
    paging::flush_tlb_page(va);
    free_test_va(va);
    pmm::free_page(phys);
}

TEST(paging_test, map_already_mapped_returns_error) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    ASSERT_EQ(paging::map_page(va, phys, paging::PAGE_KERNEL_RW, root), paging::OK);

    int32_t result = paging::map_page(va, phys, paging::PAGE_KERNEL_RW, root);
    EXPECT_EQ(result, paging::ERR_ALREADY_MAPPED);

    paging::unmap_page(va, root);
    paging::flush_tlb_page(va);
    free_test_va(va);
    pmm::free_page(phys);
}

TEST(paging_test, unmap_unmapped_is_idempotent) {
    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    int32_t result = paging::unmap_page(va, root);
    EXPECT_EQ(result, paging::OK);

    free_test_va(va);
}

TEST(paging_test, map_pages_multi) {
    constexpr size_t N = 4;
    pmm::phys_addr_t phys = pmm::alloc_pages(2); // order 2 = 4 pages
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va(N);
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    ASSERT_EQ(paging::map_pages(va, phys, paging::PAGE_KERNEL_RW, N, root), paging::OK);

    for (size_t i = 0; i < N; i++) {
        EXPECT_TRUE(paging::is_mapped(va + i * paging::PAGE_SIZE_4KB, root));
    }

    // Write pattern to each page
    for (size_t i = 0; i < N; i++) {
        auto* ptr = reinterpret_cast<volatile uint64_t*>(va + i * paging::PAGE_SIZE_4KB);
        *ptr = 0xAAAA0000ULL + i;
    }
    for (size_t i = 0; i < N; i++) {
        auto* ptr = reinterpret_cast<volatile uint64_t*>(va + i * paging::PAGE_SIZE_4KB);
        EXPECT_EQ(*ptr, 0xAAAA0000ULL + i);
    }

    paging::unmap_pages(va, N, root);
    paging::flush_tlb_range(va, va + N * paging::PAGE_SIZE_4KB);
    free_test_va(va);
    pmm::free_pages(phys, 2);
}

TEST(paging_test, map_with_device_flags) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    paging::virt_addr_t va = alloc_test_va();
    ASSERT_NE(va, static_cast<paging::virt_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    paging::page_flags_t map_flags = paging::PAGE_KERNEL_RW | paging::PAGE_DEVICE;
    ASSERT_EQ(paging::map_page(va, phys, map_flags, root), paging::OK);

    paging::page_flags_t flags = paging::get_page_flags(va, root);
    EXPECT_TRUE((flags & paging::PAGE_DEVICE) != 0);

    paging::unmap_page(va, root);
    paging::flush_tlb_page(va);
    free_test_va(va);
    pmm::free_page(phys);
}

TEST(paging_test, alignment_error) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    pmm::phys_addr_t root = paging::get_kernel_pt_root();
    // Non-page-aligned VA
    int32_t result = paging::map_page(0xDEAD, phys, paging::PAGE_KERNEL_RW, root);
    EXPECT_EQ(result, paging::ERR_ALIGNMENT);

    pmm::free_page(phys);
}

TEST(paging_test, phys_to_virt_consistency) {
    pmm::phys_addr_t phys = pmm::alloc_page();
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    void* virt = paging::phys_to_virt(phys);
    uintptr_t expected = phys + g_boot_info.hhdm_offset;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(virt), expected);

    pmm::free_page(phys);
}
