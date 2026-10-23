#define STLX_TEST_TIER TIER_MM_CORE

#include "stlx_unit_test.h"
#include "helpers.h"
#include "mm/device_backing.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/page_quarantine.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/atomic.h"
#include "sync/mutex.h"

using test_helpers::spin_wait_ge;

TEST_SUITE(device_backing_test);

// The owner of frames a test maps as device memory, freeing them and itself when released and recording the release
class test_backing final : public mm::device_backing {
public:
    test_backing(pmm::phys_addr_t frames, uint8_t order) : m_frames(frames), m_order(order) {}

    pmm::phys_addr_t frames() const { return m_frames; }

private:
    void release_memory() override;

    pmm::phys_addr_t m_frames;
    uint8_t          m_order;
};

constexpr size_t    PAGE            = pmm::PAGE_SIZE;
constexpr uint32_t  PROT_READ_WRITE = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
constexpr uintptr_t MAP_START       = mm::MMAP_BASE_DEFAULT;

static uint64_t               g_initial_free_pages = 0;
static sync::atomic<uint32_t> g_releases;
static sched::task*           g_releasing_task = nullptr;

void test_backing::release_memory() {
    pmm::free_pages(m_frames, m_order);
    heap::kfree_delete(this);

    g_releasing_task = sched::current();
    g_releases.fetch_add_release(1);
}

static int32_t device_backing_before_all() {
    page_quarantine::drain();
    g_initial_free_pages = pmm::free_page_count();
    return 0;
}

static int32_t device_backing_after_all() {
    page_quarantine::drain();
    if (pmm::free_page_count() != g_initial_free_pages) {
        return -1;
    }

    return 0;
}

BEFORE_ALL(device_backing_test, device_backing_before_all);
AFTER_ALL(device_backing_test, device_backing_after_all);

// A backing of `1 << order` contiguous frames, held by the returned reference, which is empty when an allocation fails
static rc::strong_ref<test_backing> create_backing(uint8_t order) {
    pmm::phys_addr_t frames = pmm::alloc_pages(order);
    if (frames == 0) {
        return {};
    }

    test_backing* backing = heap::kalloc_new<test_backing>(frames, order);
    if (!backing) {
        pmm::free_pages(frames, order);
        return {};
    }

    return rc::strong_ref<test_backing>::adopt(backing);
}

// Maps `page_count` pages of the backing's memory, from page `first_page` on, at the fixed address `addr`
static int32_t map_backing_pages(
    mm::mm_context* mm_ctx,
    test_backing* backing,
    size_t first_page,
    size_t page_count,
    uintptr_t addr
) {
    uint64_t offset = first_page * PAGE;
    uintptr_t mapped_addr = 0;
    return mm::mm_context_map_device(
        mm_ctx,
        backing->frames() + offset,
        page_count * PAGE,
        backing,
        offset,
        PROT_READ_WRITE,
        paging::PAGE_NORMAL,
        mm::MM_MAP_SHARED | mm::MM_MAP_FIXED,
        addr,
        &mapped_addr
    );
}

// Maps the page at `frame`, which no backing owns, at the fixed address `addr`
static int32_t map_unowned_page(mm::mm_context* mm_ctx, pmm::phys_addr_t frame, uintptr_t addr) {
    uintptr_t mapped_addr = 0;
    return mm::mm_context_map_device(
        mm_ctx,
        frame,
        PAGE,
        nullptr,
        0,
        PROT_READ_WRITE,
        paging::PAGE_NORMAL,
        mm::MM_MAP_SHARED | mm::MM_MAP_FIXED,
        addr,
        &mapped_addr
    );
}

TEST(device_backing_test, munmap_releases_backing_in_reaper) {
    g_releases.store_relaxed(0);
    g_releasing_task = nullptr;
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    rc::strong_ref<test_backing> backing = create_backing(0);
    ASSERT_NOT_NULL(backing.ptr());

    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 0, 1, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(backing->ref_count(), 2u);

    test_backing* mapped_backing = backing.ptr();
    backing.reset();
    EXPECT_EQ(mapped_backing->ref_count(), 1u);

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, MAP_START, PAGE), mm::MM_CTX_OK);
    ASSERT_TRUE(spin_wait_ge(g_releases, 1));
    EXPECT_STREQ(g_releasing_task->name, "reaper");

    mm::mm_context_release(mm_ctx);
}

TEST(device_backing_test, context_release_releases_backing) {
    g_releases.store_relaxed(0);
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    rc::strong_ref<test_backing> backing = create_backing(0);
    ASSERT_NOT_NULL(backing.ptr());

    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 0, 1, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(backing->ref_count(), 2u);
    backing.reset();

    mm::mm_context_release(mm_ctx);
    EXPECT_TRUE(spin_wait_ge(g_releases, 1));
}

TEST(device_backing_test, mappings_of_different_owners_stay_apart) {
    g_releases.store_relaxed(0);
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    rc::strong_ref<test_backing> first = create_backing(0);
    ASSERT_NOT_NULL(first.ptr());
    rc::strong_ref<test_backing> second = create_backing(1);
    ASSERT_NOT_NULL(second.ptr());
    pmm::phys_addr_t unowned_frames = pmm::alloc_pages(2);
    ASSERT_NE(unowned_frames, static_cast<pmm::phys_addr_t>(0));

    // The first two pages, both unowned, merge, while each owned page stays apart from the unowned page beside it
    // and from the other owner's page, even though the second owner's page starts at the offset where the first's ends
    ASSERT_EQ(map_unowned_page(mm_ctx, unowned_frames, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(map_unowned_page(mm_ctx, unowned_frames + PAGE, MAP_START + PAGE), mm::MM_CTX_OK);
    ASSERT_EQ(map_backing_pages(mm_ctx, first.ptr(), 0, 1, MAP_START + 2 * PAGE), mm::MM_CTX_OK);
    ASSERT_EQ(map_backing_pages(mm_ctx, second.ptr(), 1, 1, MAP_START + 3 * PAGE), mm::MM_CTX_OK);
    ASSERT_EQ(map_unowned_page(mm_ctx, unowned_frames + 2 * PAGE, MAP_START + 4 * PAGE), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), 4u);
    ASSERT_EQ(first->ref_count(), 2u);
    ASSERT_EQ(second->ref_count(), 2u);

    first.reset();
    second.reset();
    mm::mm_context_release(mm_ctx);
    EXPECT_TRUE(spin_wait_ge(g_releases, 2));
    pmm::free_pages(unowned_frames, 2);
}

TEST(device_backing_test, same_owner_merges_only_where_offsets_continue) {
    g_releases.store_relaxed(0);
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    rc::strong_ref<test_backing> backing = create_backing(1);
    ASSERT_NOT_NULL(backing.ptr());

    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 1, 1, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(backing->ref_count(), 2u);
    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 0, 1, MAP_START + PAGE), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), 2u);

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, MAP_START, 2 * PAGE), mm::MM_CTX_OK);
    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 0, 1, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 1, 1, MAP_START + PAGE), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), 1u);
    EXPECT_EQ(backing->ref_count(), 2u);

    backing.reset();
    mm::mm_context_release(mm_ctx);
    EXPECT_TRUE(spin_wait_ge(g_releases, 1));
}

TEST(device_backing_test, partial_munmap_keeps_backing) {
    g_releases.store_relaxed(0);
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    rc::strong_ref<test_backing> backing = create_backing(2);
    ASSERT_NOT_NULL(backing.ptr());

    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 1, 3, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(backing->ref_count(), 2u);

    test_backing* mapped_backing = backing.ptr();
    backing.reset();

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, MAP_START + PAGE, PAGE), mm::MM_CTX_OK);
    ASSERT_EQ(mapped_backing->ref_count(), 2u);

    // The part after the hole keeps the owner and starts at the owner's fourth page
    sync::mutex_lock(mm_ctx->lock);

    mm::vma* tail = mm::vma_find_locked(mm_ctx, MAP_START + 2 * PAGE);
    mm::device_backing* tail_backing = tail ? tail->device_backing_ref.ptr() : nullptr;
    uint64_t tail_offset = tail ? tail->backing_offset : 0;
    sync::mutex_unlock(mm_ctx->lock);

    EXPECT_TRUE(tail_backing == mapped_backing);
    EXPECT_EQ(tail_offset, 3 * PAGE);

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, MAP_START, PAGE), mm::MM_CTX_OK);
    EXPECT_EQ(mapped_backing->ref_count(), 1u);

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, MAP_START + 2 * PAGE, PAGE), mm::MM_CTX_OK);
    EXPECT_TRUE(spin_wait_ge(g_releases, 1));

    mm::mm_context_release(mm_ctx);
}

TEST(device_backing_test, shrinking_remap_keeps_backing) {
    g_releases.store_relaxed(0);
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    rc::strong_ref<test_backing> backing = create_backing(1);
    ASSERT_NOT_NULL(backing.ptr());

    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 0, 2, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(backing->ref_count(), 2u);

    test_backing* mapped_backing = backing.ptr();
    backing.reset();

    uintptr_t remapped_addr = 0;
    ASSERT_EQ(mm::mm_context_remap(mm_ctx, MAP_START, 2 * PAGE, PAGE, 0, 0, &remapped_addr), mm::MM_CTX_OK);
    EXPECT_EQ(remapped_addr, MAP_START);
    EXPECT_EQ(mapped_backing->ref_count(), 1u);

    ASSERT_EQ(mm::mm_context_unmap(mm_ctx, MAP_START, PAGE), mm::MM_CTX_OK);
    EXPECT_TRUE(spin_wait_ge(g_releases, 1));

    mm::mm_context_release(mm_ctx);
}

TEST(device_backing_test, protection_split_holds_reference_per_part) {
    g_releases.store_relaxed(0);
    mm::mm_context* mm_ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(mm_ctx);
    rc::strong_ref<test_backing> backing = create_backing(2);
    ASSERT_NOT_NULL(backing.ptr());

    ASSERT_EQ(map_backing_pages(mm_ctx, backing.ptr(), 0, 4, MAP_START), mm::MM_CTX_OK);
    ASSERT_EQ(backing->ref_count(), 2u);

    test_backing* mapped_backing = backing.ptr();
    backing.reset();

    ASSERT_EQ(mm::mm_context_mprotect(mm_ctx, MAP_START + PAGE, PAGE, mm::MM_PROT_READ), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), 3u);
    EXPECT_EQ(mapped_backing->ref_count(), 3u);

    ASSERT_EQ(mm::mm_context_mprotect(mm_ctx, MAP_START + PAGE, PAGE, PROT_READ_WRITE), mm::MM_CTX_OK);
    EXPECT_EQ(mm::mm_context_vma_count(mm_ctx), 1u);
    EXPECT_EQ(mapped_backing->ref_count(), 1u);

    mm::mm_context_release(mm_ctx);
    EXPECT_TRUE(spin_wait_ge(g_releases, 1));
}
