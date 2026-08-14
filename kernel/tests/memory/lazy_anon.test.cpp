#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "mm/mm.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "common/string.h"

TEST_SUITE(lazy_anon);

static uint64_t g_initial_free_pages = 0;

static int32_t lazy_anon_before_all() {
    g_initial_free_pages = pmm::free_page_count();
    return 0;
}

static int32_t lazy_anon_after_all() {
    return pmm::free_page_count() == g_initial_free_pages ? 0 : -1;
}

BEFORE_ALL(lazy_anon, lazy_anon_before_all);
AFTER_ALL(lazy_anon, lazy_anon_after_all);

static constexpr size_t PAGE = pmm::PAGE_SIZE;
static constexpr uint32_t LAZY_ANON =
    mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS | mm::MM_MAP_LAZY;

// --- lazy_map_reserves_without_pages ---
// Proves: a lazy anonymous mapping consumes address space but no
// physical pages until touched.

TEST(lazy_anon, lazy_map_reserves_without_pages) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uint64_t before = pmm::free_page_count();
    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 16 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);
    EXPECT_NE(addr, static_cast<uintptr_t>(0));
    EXPECT_EQ(pmm::free_page_count(), before);
    EXPECT_EQ(paging::get_physical(addr, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));

    mm::mm_context_release(mm_ctx);
}

// --- fault_populates_lazy_page ---
// Proves: a demand fault on a lazy anonymous page allocates, zeroes,
// and maps exactly one page.

TEST(lazy_anon, fault_populates_lazy_page) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 4 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);

    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr + PAGE + 128, 0));

    pmm::phys_addr_t phys = paging::get_physical(addr + PAGE, mm_ctx->pt_root);
    ASSERT_NE(phys, static_cast<pmm::phys_addr_t>(0));

    const uint8_t* data =
        static_cast<const uint8_t*>(paging::phys_to_virt(phys));
    bool all_zero = true;
    for (size_t i = 0; i < PAGE; i++) {
        if (data[i] != 0) {
            all_zero = false;
            break;
        }
    }
    EXPECT_TRUE(all_zero);

    // Neighboring pages stay absent
    EXPECT_EQ(paging::get_physical(addr, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));
    EXPECT_EQ(paging::get_physical(addr + 2 * PAGE, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));

    mm::mm_context_release(mm_ctx);
}

// --- prot_none_reservation_never_faults_in ---
// Proves: a no-access reservation refuses demand faults until a later
// mprotect grants access, then faults in with the new protection.

TEST(lazy_anon, prot_none_reservation_never_faults_in) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 8 * PAGE, 0, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);

    EXPECT_FALSE(mm::handle_user_pf(mm_ctx, addr, 0));
    EXPECT_EQ(paging::get_physical(addr, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));

    // Commit part of the reservation, reserve-then-commit style
    ASSERT_EQ(mm::mm_context_mprotect(
        mm_ctx, addr, 2 * PAGE, mm::MM_PROT_READ | mm::MM_PROT_WRITE
    ), mm::MM_CTX_OK);

    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr, 0));
    EXPECT_NE(paging::get_physical(addr, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));

    // The uncommitted tail still refuses faults
    EXPECT_FALSE(mm::handle_user_pf(mm_ctx, addr + 4 * PAGE, 0));

    mm::mm_context_release(mm_ctx);
}

// --- access_type_must_match_region_protection ---
// Proves: a fault whose access type the region forbids is rejected
// before any physical page is committed.

TEST(lazy_anon, access_type_must_match_region_protection) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 2 * PAGE, mm::MM_PROT_READ, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);

    // Writes and instruction fetches on a read-only region commit nothing
    EXPECT_FALSE(mm::handle_user_pf(mm_ctx, addr, mm::PF_FLAG_WRITE));
    EXPECT_FALSE(mm::handle_user_pf(mm_ctx, addr, mm::PF_FLAG_INSTRUCTION));
    EXPECT_EQ(paging::get_physical(addr, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));

    // A plain read faults in as usual
    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr, 0));
    EXPECT_NE(paging::get_physical(addr, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));

    mm::mm_context_release(mm_ctx);
}

// --- mprotect_splits_and_coalesces ---
// Proves: protecting the middle of a mapping splits the VMA and
// restoring the protection coalesces it back into one.

static int count_vmas(mm::mm_context* mm_ctx) {
    int count = 0;
    mm::vma* cur = mm_ctx->vmas.min();
    while (cur) {
        count++;
        cur = mm_ctx->vmas.next(*cur);
    }
    return count;
}

TEST(lazy_anon, mprotect_splits_and_coalesces) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 4 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);
    int base_count = count_vmas(mm_ctx);

    ASSERT_EQ(mm::mm_context_mprotect(
        mm_ctx, addr + PAGE, 2 * PAGE, mm::MM_PROT_READ
    ), mm::MM_CTX_OK);
    EXPECT_EQ(count_vmas(mm_ctx), base_count + 2);

    ASSERT_EQ(mm::mm_context_mprotect(
        mm_ctx, addr + PAGE, 2 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE
    ), mm::MM_CTX_OK);
    EXPECT_EQ(count_vmas(mm_ctx), base_count);

    mm::mm_context_release(mm_ctx);
}

// --- mprotect_changes_present_pages ---
// Proves: protection changes apply to already-faulted pages and to
// pages faulted in afterward.

TEST(lazy_anon, mprotect_changes_present_pages) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 2 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);

    // One page present, one still absent, then flip both read-only
    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr, 0));
    ASSERT_EQ(mm::mm_context_mprotect(
        mm_ctx, addr, 2 * PAGE, mm::MM_PROT_READ
    ), mm::MM_CTX_OK);

    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr + PAGE, 0));
    EXPECT_NE(paging::get_physical(addr + PAGE, mm_ctx->pt_root),
              static_cast<pmm::phys_addr_t>(0));

    mm::mm_context_release(mm_ctx);
}

// --- partial_munmap_punches_hole ---
// Proves: unmapping the middle of a lazy reservation leaves both ends
// faultable and the hole dead.

TEST(lazy_anon, partial_munmap_punches_hole) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, 6 * PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, addr + 2 * PAGE, 2 * PAGE),
              mm::MM_CTX_OK);

    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr, 0));
    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr + 5 * PAGE, 0));
    EXPECT_FALSE(mm::handle_user_pf(mm_ctx, addr + 3 * PAGE, 0));

    mm::mm_context_release(mm_ctx);
}

// --- present_fault_is_fatal ---
// Proves: faults on present pages, which are protection violations,
// are never resolved by demand paging.

TEST(lazy_anon, present_fault_is_fatal) {
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);

    uintptr_t addr = 0;
    ASSERT_EQ(mm::mm_context_map_anonymous(
        mm_ctx, 0, PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE, LAZY_ANON, &addr
    ), mm::MM_CTX_OK);

    EXPECT_TRUE(mm::handle_user_pf(mm_ctx, addr, 0));
    EXPECT_FALSE(mm::handle_user_pf(mm_ctx, addr, mm::PF_FLAG_PRESENT));

    mm::mm_context_release(mm_ctx);
}
