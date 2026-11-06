#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/virtio_gpu.h"
#include "drivers/graphics/virtio_gpu_backend.h"

TEST_SUITE(virtio_gpu_backend);

using drivers::context_id;
using drivers::context_id_allocator;
using drivers::list_page_runs;
using drivers::region_allocator;
using drivers::region_range;
using drivers::virtio::virtio_gpu_mem_entry;

static constexpr uint64_t PAGE_BYTES = 0x1000;

TEST(virtio_gpu_backend, region_ranges_take_the_lowest_offset_with_room) {
    region_allocator region = {};
    region.init(8 * PAGE_BYTES);

    region_range first = {};
    region_range second = {};
    region_range third = {};
    ASSERT_TRUE(region.allocate(2 * PAGE_BYTES, first));
    ASSERT_TRUE(region.allocate(PAGE_BYTES, second));
    ASSERT_TRUE(region.allocate(PAGE_BYTES, third));

    EXPECT_EQ(first.offset, 0u);
    EXPECT_EQ(second.offset, 2 * PAGE_BYTES);
    EXPECT_EQ(third.offset, 3 * PAGE_BYTES);

    region.release(second);

    region_range reused = {};
    ASSERT_TRUE(region.allocate(PAGE_BYTES, reused));
    EXPECT_EQ(reused.offset, 2 * PAGE_BYTES);
    EXPECT_EQ(reused.size, PAGE_BYTES);
}

TEST(virtio_gpu_backend, a_region_range_passes_over_a_gap_too_small_for_it) {
    region_allocator region = {};
    region.init(8 * PAGE_BYTES);

    region_range first = {};
    region_range second = {};
    region_range third = {};
    ASSERT_TRUE(region.allocate(PAGE_BYTES, first));
    ASSERT_TRUE(region.allocate(PAGE_BYTES, second));
    ASSERT_TRUE(region.allocate(PAGE_BYTES, third));

    region.release(second);

    region_range wide = {};
    ASSERT_TRUE(region.allocate(2 * PAGE_BYTES, wide));
    EXPECT_EQ(wide.offset, 3 * PAGE_BYTES);

    region_range narrow = {};
    ASSERT_TRUE(region.allocate(PAGE_BYTES, narrow));
    EXPECT_EQ(narrow.offset, PAGE_BYTES);

    region_range past_every_range = {};
    ASSERT_TRUE(region.allocate(PAGE_BYTES, past_every_range));
    EXPECT_EQ(past_every_range.offset, 5 * PAGE_BYTES);
}

TEST(virtio_gpu_backend, region_ranges_reuse_the_lowest_gap_whichever_was_released_first) {
    region_allocator region = {};
    region.init(4 * PAGE_BYTES);

    region_range ranges[4] = {};
    for (region_range& range : ranges) {
        ASSERT_TRUE(region.allocate(PAGE_BYTES, range));
    }

    region.release(ranges[0]);
    region.release(ranges[2]);

    region_range lower_released_first = {};
    ASSERT_TRUE(region.allocate(PAGE_BYTES, lower_released_first));
    EXPECT_EQ(lower_released_first.offset, 0u);

    region.release(lower_released_first);

    region_range lower_released_last = {};
    ASSERT_TRUE(region.allocate(PAGE_BYTES, lower_released_last));
    EXPECT_EQ(lower_released_last.offset, 0u);
}

TEST(virtio_gpu_backend, a_region_fills_to_its_last_whole_page_and_refuses_past_it) {
    region_allocator region = {};
    region.init(4 * PAGE_BYTES + 100);

    region_range most = {};
    ASSERT_TRUE(region.allocate(3 * PAGE_BYTES, most));

    region_range too_wide = {};
    EXPECT_FALSE(region.allocate(2 * PAGE_BYTES, too_wide));

    region_range last = {};
    ASSERT_TRUE(region.allocate(PAGE_BYTES, last));
    EXPECT_EQ(last.offset, 3 * PAGE_BYTES);

    region_range none = {};
    EXPECT_FALSE(region.allocate(PAGE_BYTES, none));

    region.release(most);
    region_range refilled = {};
    ASSERT_TRUE(region.allocate(3 * PAGE_BYTES, refilled));
    EXPECT_EQ(refilled.offset, 0u);
}

TEST(virtio_gpu_backend, context_ids_count_up_from_1) {
    context_id_allocator ids = {};
    ids.init(0xFFFFFFFF);

    context_id first = {};
    context_id second = {};
    ASSERT_TRUE(ids.take(first));
    ASSERT_TRUE(ids.take(second));
    EXPECT_EQ(first.value, 1u);
    EXPECT_EQ(second.value, 2u);
}

TEST(virtio_gpu_backend, context_ids_wrap_past_0_and_skip_every_held_id) {
    context_id_allocator ids = {};
    ids.init(3);

    context_id one = {};
    context_id two = {};
    context_id three = {};
    ASSERT_TRUE(ids.take(one));
    ASSERT_TRUE(ids.take(two));
    ASSERT_TRUE(ids.take(three));

    EXPECT_EQ(three.value, 3u);

    ids.give_back(two);

    context_id after_wrap = {};
    ASSERT_TRUE(ids.take(after_wrap));
    EXPECT_EQ(after_wrap.value, 2u);
}

TEST(virtio_gpu_backend, context_ids_run_out_while_every_id_is_held_and_return_once_given_back) {
    context_id_allocator ids = {};
    ids.init(2);

    context_id one = {};
    context_id two = {};
    ASSERT_TRUE(ids.take(one));
    ASSERT_TRUE(ids.take(two));

    context_id refused = {};
    EXPECT_FALSE(ids.take(refused));

    ids.give_back(one);

    context_id returned = {};
    ASSERT_TRUE(ids.take(returned));
    EXPECT_EQ(returned.value, 1u);
}

TEST(virtio_gpu_backend, a_given_back_context_id_waits_for_the_counter_to_wrap) {
    context_id_allocator ids = {};
    ids.init(0xFFFFFFFF);

    context_id one = {};
    context_id two = {};
    ASSERT_TRUE(ids.take(one));
    ASSERT_TRUE(ids.take(two));
    ids.give_back(one);

    context_id next = {};
    ASSERT_TRUE(ids.take(next));
    EXPECT_EQ(next.value, 3u);
}

TEST(virtio_gpu_backend, page_runs_join_contiguous_pages_and_split_at_a_gap) {
    pmm::phys_addr_t pages[] = {16 * PAGE_BYTES, 17 * PAGE_BYTES, 19 * PAGE_BYTES};
    virtio_gpu_mem_entry entries[3] = {};

    ASSERT_EQ(list_page_runs(pages, 3, entries, 3), static_cast<size_t>(2));
    EXPECT_EQ(static_cast<uint64_t>(entries[0].addr), 16 * PAGE_BYTES);
    EXPECT_EQ(static_cast<uint64_t>(entries[0].length), 2 * PAGE_BYTES);
    EXPECT_EQ(static_cast<uint64_t>(entries[1].addr), 19 * PAGE_BYTES);
    EXPECT_EQ(static_cast<uint64_t>(entries[1].length), PAGE_BYTES);
}

TEST(virtio_gpu_backend, page_runs_refuse_one_run_past_their_capacity) {
    pmm::phys_addr_t pages[] = {16 * PAGE_BYTES, 18 * PAGE_BYTES, 20 * PAGE_BYTES};
    virtio_gpu_mem_entry entries[3] = {};

    EXPECT_EQ(list_page_runs(pages, 3, entries, 2), static_cast<size_t>(0));
    EXPECT_EQ(list_page_runs(pages, 3, entries, 3), static_cast<size_t>(3));
}
