#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "mm/shmem.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "common/string.h"
#include "sync/mutex.h"

TEST_SUITE(shmem_test);

namespace {

uint64_t g_initial_free_pages = 0;

int32_t shmem_before_all() {
    g_initial_free_pages = pmm::free_page_count();
    return 0;
}

int32_t shmem_after_all() {
    uint64_t final_free = pmm::free_page_count();
    if (final_free != g_initial_free_pages) {
        return -1;
    }
    return 0;
}

constexpr size_t PAGE = pmm::PAGE_SIZE;

} // namespace

BEFORE_ALL(shmem_test, shmem_before_all);
AFTER_ALL(shmem_test, shmem_after_all);

TEST(shmem_test, create_and_destroy) {
    uint64_t before = pmm::free_page_count();
    mm::shmem* s = mm::shmem_create(0);
    ASSERT_NOT_NULL(s);
    EXPECT_EQ(s->m_size, static_cast<size_t>(0));
    EXPECT_EQ(s->m_page_count, static_cast<size_t>(0));

    mm::shmem::ref_destroy(s);
    uint64_t after = pmm::free_page_count();
    EXPECT_EQ(after, before);
}

TEST(shmem_test, create_with_initial_size) {
    uint64_t before = pmm::free_page_count();
    mm::shmem* s = mm::shmem_create(2 * PAGE);
    ASSERT_NOT_NULL(s);
    EXPECT_EQ(s->m_size, 2 * PAGE);
    EXPECT_EQ(s->m_page_count, static_cast<size_t>(2));

    sync::mutex_lock(s->lock);
    pmm::phys_addr_t p0 = mm::shmem_get_page_locked(s, 0);
    pmm::phys_addr_t p1 = mm::shmem_get_page_locked(s, 1);
    pmm::phys_addr_t p2 = mm::shmem_get_page_locked(s, 2);
    sync::mutex_unlock(s->lock);

    EXPECT_NE(p0, static_cast<pmm::phys_addr_t>(0));
    EXPECT_NE(p1, static_cast<pmm::phys_addr_t>(0));
    EXPECT_EQ(p2, static_cast<pmm::phys_addr_t>(0));

    mm::shmem::ref_destroy(s);
    EXPECT_EQ(pmm::free_page_count(), before);
}

TEST(shmem_test, resize_grow_and_shrink) {
    mm::shmem* s = mm::shmem_create(PAGE);
    ASSERT_NOT_NULL(s);

    sync::mutex_lock(s->lock);
    ASSERT_EQ(mm::shmem_resize_locked(s, 3 * PAGE), mm::SHMEM_OK);
    EXPECT_EQ(s->m_page_count, static_cast<size_t>(3));
    EXPECT_EQ(s->m_size, 3 * PAGE);

    ASSERT_EQ(mm::shmem_resize_locked(s, PAGE), mm::SHMEM_OK);
    EXPECT_EQ(s->m_page_count, static_cast<size_t>(1));
    EXPECT_EQ(s->m_size, PAGE);
    sync::mutex_unlock(s->lock);

    mm::shmem::ref_destroy(s);
}

TEST(shmem_test, read_write_roundtrip) {
    mm::shmem* s = mm::shmem_create(PAGE);
    ASSERT_NOT_NULL(s);

    uint8_t wbuf[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    ssize_t written = mm::shmem_write(s, 0, wbuf, 8);
    EXPECT_EQ(written, static_cast<ssize_t>(8));

    uint8_t rbuf[8] = {};
    ssize_t rd = mm::shmem_read(s, 0, rbuf, 8);
    EXPECT_EQ(rd, static_cast<ssize_t>(8));
    EXPECT_EQ(string::memcmp(wbuf, rbuf, 8), 0);

    mm::shmem::ref_destroy(s);
}

TEST(shmem_test, shared_map_two_contexts_same_backing) {
    uint64_t before = pmm::free_page_count();

    mm::shmem* s = mm::shmem_create(PAGE);
    ASSERT_NOT_NULL(s);

    uint8_t pattern[4] = {0x11, 0x22, 0x33, 0x44};
    mm::shmem_write(s, 0, pattern, 4);

    mm::mm_context* ctx_a = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx_a);
    mm::mm_context* ctx_b = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx_b);

    uintptr_t addr_a = 0;
    int32_t rc = mm::mm_context_map_shared(
        ctx_a, s, 0, PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        mm::MM_MAP_SHARED, 0, &addr_a);
    ASSERT_EQ(rc, mm::MM_CTX_OK);
    EXPECT_NE(addr_a, static_cast<uintptr_t>(0));

    uintptr_t addr_b = 0;
    rc = mm::mm_context_map_shared(
        ctx_b, s, 0, PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        mm::MM_MAP_SHARED, 0, &addr_b);
    ASSERT_EQ(rc, mm::MM_CTX_OK);
    EXPECT_NE(addr_b, static_cast<uintptr_t>(0));

    pmm::phys_addr_t phys_a = paging::get_physical(addr_a, ctx_a->pt_root);
    pmm::phys_addr_t phys_b = paging::get_physical(addr_b, ctx_b->pt_root);
    EXPECT_EQ(phys_a, phys_b);

    EXPECT_EQ(mm::mm_context_vma_count(ctx_a), static_cast<size_t>(1));
    EXPECT_EQ(mm::mm_context_vma_count(ctx_b), static_cast<size_t>(1));

    mm::mm_context_release(ctx_a);

    sync::mutex_lock(s->lock);
    pmm::phys_addr_t still_valid = mm::shmem_get_page_locked(s, 0);
    sync::mutex_unlock(s->lock);
    EXPECT_NE(still_valid, static_cast<pmm::phys_addr_t>(0));

    phys_b = paging::get_physical(addr_b, ctx_b->pt_root);
    EXPECT_EQ(phys_b, still_valid);

    mm::mm_context_release(ctx_b);
    mm::shmem::ref_destroy(s);

    EXPECT_EQ(pmm::free_page_count(), before);
}

TEST(shmem_test, unmap_shared_does_not_free_pages) {
    uint64_t before = pmm::free_page_count();

    mm::shmem* s = mm::shmem_create(PAGE);
    ASSERT_NOT_NULL(s);

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t addr = 0;
    int32_t rc = mm::mm_context_map_shared(
        ctx, s, 0, PAGE,
        mm::MM_PROT_READ | mm::MM_PROT_WRITE,
        mm::MM_MAP_SHARED, 0, &addr);
    ASSERT_EQ(rc, mm::MM_CTX_OK);

    rc = mm::mm_context_unmap(ctx, addr, PAGE);
    ASSERT_EQ(rc, mm::MM_CTX_OK);

    sync::mutex_lock(s->lock);
    pmm::phys_addr_t still_valid = mm::shmem_get_page_locked(s, 0);
    sync::mutex_unlock(s->lock);
    EXPECT_NE(still_valid, static_cast<pmm::phys_addr_t>(0));

    mm::mm_context_release(ctx);
    mm::shmem::ref_destroy(s);

    EXPECT_EQ(pmm::free_page_count(), before);
}

TEST(shmem_test, map_shared_rejects_hole) {
    mm::shmem* s = mm::shmem_create(0);
    ASSERT_NOT_NULL(s);

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t addr = 0;
    int32_t rc = mm::mm_context_map_shared(
        ctx, s, 0, PAGE,
        mm::MM_PROT_READ, mm::MM_MAP_SHARED, 0, &addr);
    EXPECT_NE(rc, mm::MM_CTX_OK);

    mm::mm_context_release(ctx);
    mm::shmem::ref_destroy(s);
}
