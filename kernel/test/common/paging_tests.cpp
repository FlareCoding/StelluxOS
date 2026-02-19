#include "test/framework/test_framework.h"
#include "mm/paging.h"
#include "mm/kva.h"
#include "mm/pmm.h"

namespace {

int32_t alloc_test_va(kva::allocation& out) {
    return kva::alloc(
        pmm::PAGE_SIZE,
        pmm::PAGE_SIZE,
        0,
        0,
        kva::placement::low,
        kva::tag::boot,
        0,
        out
    );
}

} // namespace

STLX_TEST_SUITE(mm_paging, test::phase::post_mm);

STLX_TEST(mm_paging, map_get_unmap_roundtrip) {
    kva::allocation kva_alloc = {};
    pmm::phys_addr_t phys = 0;

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    STLX_ASSERT_EQ(ctx, alloc_test_va(kva_alloc), kva::OK);

    phys = pmm::alloc_page();
    STLX_ASSERT_NE(ctx, phys, static_cast<pmm::phys_addr_t>(0));

    STLX_ASSERT_EQ(ctx, paging::map_page(kva_alloc.base, phys, paging::PAGE_KERNEL_RW, root), paging::OK);

    STLX_ASSERT_TRUE(ctx, paging::is_mapped(kva_alloc.base, root));
    STLX_ASSERT_EQ(ctx, paging::get_physical(kva_alloc.base, root), phys);

    paging::page_flags_t flags = paging::get_page_flags(kva_alloc.base, root);
    STLX_ASSERT_TRUE(ctx, (flags & paging::PAGE_READ) != 0);
    STLX_ASSERT_TRUE(ctx, (flags & paging::PAGE_WRITE) != 0);

    STLX_ASSERT_EQ(ctx, paging::unmap_page(kva_alloc.base, root), paging::OK);

    STLX_ASSERT_FALSE(ctx, paging::is_mapped(kva_alloc.base, root));
    STLX_ASSERT_EQ(ctx, paging::unmap_page(kva_alloc.base, root), paging::OK); // idempotent

    STLX_ASSERT_EQ(ctx, pmm::free_page(phys), pmm::OK);
    STLX_ASSERT_EQ(ctx, kva::free(kva_alloc.base), kva::OK);
}

STLX_TEST(mm_paging, set_page_flags_updates_permissions) {
    kva::allocation kva_alloc = {};
    pmm::phys_addr_t phys = 0;

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    STLX_ASSERT_EQ(ctx, alloc_test_va(kva_alloc), kva::OK);

    phys = pmm::alloc_page();
    STLX_ASSERT_NE(ctx, phys, static_cast<pmm::phys_addr_t>(0));

    STLX_ASSERT_EQ(ctx, paging::map_page(kva_alloc.base, phys, paging::PAGE_KERNEL_RW, root), paging::OK);

    STLX_ASSERT_EQ(ctx, paging::set_page_flags(kva_alloc.base, paging::PAGE_KERNEL_RO, root), paging::OK);

    paging::page_flags_t flags = paging::get_page_flags(kva_alloc.base, root);
    STLX_ASSERT_TRUE(ctx, (flags & paging::PAGE_READ) != 0);
    STLX_ASSERT_FALSE(ctx, (flags & paging::PAGE_WRITE) != 0);

    STLX_ASSERT_EQ(ctx, paging::unmap_page(kva_alloc.base, root), paging::OK);
    STLX_ASSERT_EQ(ctx, pmm::free_page(phys), pmm::OK);
    STLX_ASSERT_EQ(ctx, kva::free(kva_alloc.base), kva::OK);
}

STLX_TEST(mm_paging, unaligned_arguments_are_rejected) {
    kva::allocation kva_alloc = {};
    pmm::phys_addr_t phys = 0;

    pmm::phys_addr_t root = paging::get_kernel_pt_root();

    STLX_ASSERT_EQ(ctx, alloc_test_va(kva_alloc), kva::OK);

    phys = pmm::alloc_page();
    STLX_ASSERT_NE(ctx, phys, static_cast<pmm::phys_addr_t>(0));

    STLX_ASSERT_EQ(ctx,
                   paging::map_page(kva_alloc.base + 1, phys, paging::PAGE_KERNEL_RW, root),
                   paging::ERR_ALIGNMENT);
    STLX_ASSERT_EQ(ctx,
                   paging::map_page(kva_alloc.base, phys + 1, paging::PAGE_KERNEL_RW, root),
                   paging::ERR_ALIGNMENT);
    STLX_ASSERT_EQ(ctx, pmm::free_page(phys), pmm::OK);
    STLX_ASSERT_EQ(ctx, kva::free(kva_alloc.base), kva::OK);
}
