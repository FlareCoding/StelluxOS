#include "test/framework/test_framework.h"
#include "mm/vmm.h"
#include "mm/paging.h"
#include "mm/pmm.h"

STLX_TEST_SUITE(mm_vmm, test::phase::post_mm);

STLX_TEST(mm_vmm, alloc_and_free_non_contiguous_pages) {
    uintptr_t addr = 0;
    uint8_t* bytes = nullptr;
    bool allocated = false;

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    if (!STLX_EXPECT_EQ(ctx,
                        vmm::alloc(2, paging::PAGE_KERNEL_RW, vmm::ALLOC_ZERO, kva::tag::generic, addr),
                        vmm::OK)) {
        goto cleanup;
    }
    allocated = true;

    STLX_EXPECT_TRUE(ctx, addr != 0);
    STLX_EXPECT_TRUE(ctx, paging::is_mapped(addr, root));
    STLX_EXPECT_TRUE(ctx, paging::is_mapped(addr + pmm::PAGE_SIZE, root));

    bytes = reinterpret_cast<uint8_t*>(addr);
    for (size_t i = 0; i < 64; i++) {
        STLX_EXPECT_EQ(ctx, bytes[i], static_cast<uint8_t>(0));
    }

    bytes[0] = 0xA5;
    STLX_EXPECT_EQ(ctx, bytes[0], static_cast<uint8_t>(0xA5));

    STLX_EXPECT_EQ(ctx, vmm::free(addr), vmm::OK);
    allocated = false;
    STLX_EXPECT_FALSE(ctx, paging::is_mapped(addr, root));

cleanup:
    if (allocated) {
        vmm::free(addr);
    }
}

STLX_TEST(mm_vmm, alloc_contiguous_reports_physical_base) {
    uintptr_t addr = 0;
    pmm::phys_addr_t phys = 0;
    bool allocated = false;

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    if (!STLX_EXPECT_EQ(ctx,
                        vmm::alloc_contiguous(4,
                                              pmm::ZONE_ANY,
                                              paging::PAGE_KERNEL_RW,
                                              0,
                                              kva::tag::generic,
                                              addr,
                                              phys),
                        vmm::OK)) {
        goto cleanup;
    }
    allocated = true;

    STLX_EXPECT_NE(ctx, addr, static_cast<uintptr_t>(0));
    STLX_EXPECT_NE(ctx, phys, static_cast<pmm::phys_addr_t>(0));
    STLX_EXPECT_EQ(ctx, paging::get_physical(addr, root), phys);

    STLX_EXPECT_EQ(ctx, vmm::free(addr), vmm::OK);
    allocated = false;
    STLX_EXPECT_FALSE(ctx, paging::is_mapped(addr, root));

cleanup:
    if (allocated) {
        vmm::free(addr);
    }
}

STLX_TEST(mm_vmm, alloc_stack_creates_unmapped_guard_region) {
    uintptr_t base = 0;
    uintptr_t top = 0;
    bool allocated = false;

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    if (!STLX_EXPECT_EQ(ctx,
                        vmm::alloc_stack(2, 1, kva::tag::privileged_stack, base, top),
                        vmm::OK)) {
        goto cleanup;
    }
    allocated = true;

    STLX_EXPECT_EQ(ctx, top - base, static_cast<uintptr_t>(2 * pmm::PAGE_SIZE));
    STLX_EXPECT_TRUE(ctx, paging::is_mapped(base, root));
    STLX_EXPECT_TRUE(ctx, paging::is_mapped(base + pmm::PAGE_SIZE, root));
    STLX_EXPECT_FALSE(ctx, paging::is_mapped(base - pmm::PAGE_SIZE, root));

    STLX_EXPECT_EQ(ctx, vmm::free(base), vmm::OK);
    allocated = false;

cleanup:
    if (allocated) {
        vmm::free(base);
    }
}

STLX_TEST(mm_vmm, map_phys_unmap_keeps_physical_page_owned_by_caller) {
    pmm::phys_addr_t phys = 0;
    uintptr_t map_base = 0;
    uintptr_t map_va = 0;
    bool mapped = false;
    bool phys_allocated = false;

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    phys = pmm::alloc_page();
    if (!STLX_EXPECT_NE(ctx, phys, static_cast<pmm::phys_addr_t>(0))) {
        goto cleanup;
    }
    phys_allocated = true;

    *reinterpret_cast<uint8_t*>(paging::phys_to_virt(phys)) = 0x5A;

    if (!STLX_EXPECT_EQ(ctx,
                        vmm::map_phys(phys, pmm::PAGE_SIZE, paging::PAGE_KERNEL_RW, map_base, map_va),
                        vmm::OK)) {
        goto cleanup;
    }
    mapped = true;

    STLX_EXPECT_EQ(ctx, *reinterpret_cast<uint8_t*>(map_va), static_cast<uint8_t>(0x5A));

    STLX_EXPECT_EQ(ctx, vmm::free(map_va), vmm::OK);
    mapped = false;
    STLX_EXPECT_FALSE(ctx, paging::is_mapped(map_base, root));

    STLX_EXPECT_EQ(ctx, pmm::free_page(phys), pmm::OK);
    phys_allocated = false;

cleanup:
    if (mapped) {
        vmm::free(map_va);
    }
    if (phys_allocated) {
        pmm::free_page(phys);
    }
}
