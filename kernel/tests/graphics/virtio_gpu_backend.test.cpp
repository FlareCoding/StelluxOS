#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/graphics/virtio_gpu.h"
#include "drivers/graphics/virtio_gpu_backend.h"

TEST_SUITE(virtio_gpu_backend);

using drivers::context_id;
using drivers::context_id_allocator;
using drivers::free_returned_lists;
using drivers::list_page_runs;
using drivers::region_allocator;
using drivers::region_range;
using drivers::ring_placement;
using drivers::send_ring;
using drivers::virtio::virtio_gpu_mem_entry;
using drivers::virtio_gpu_submission;
using drivers::write_placed;

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

TEST(virtio_gpu_backend, send_ring_lists_follow_one_another_without_wrapping) {
    send_ring ring = {};
    ring.init(1000);

    ring_placement first = ring.place(300);
    EXPECT_FALSE(ring.wraps(700));
    ring_placement second = ring.place(700);

    EXPECT_EQ(first.offset, 0u);
    EXPECT_EQ(first.first_bytes, 300u);
    EXPECT_EQ(first.second_bytes, 0u);

    EXPECT_EQ(second.offset, 300u);
    EXPECT_EQ(second.first_bytes, 700u);
    EXPECT_EQ(second.second_bytes, 0u);
    EXPECT_FALSE(ring.has_room(1));
}

TEST(virtio_gpu_backend, a_send_ring_list_crossing_the_end_continues_at_the_start) {
    send_ring ring = {};
    ring.init(1000);

    ring.place(600);
    ring.place(300);
    ring.free_oldest(600);

    ASSERT_TRUE(ring.has_room(400));
    EXPECT_TRUE(ring.wraps(400));
    ring_placement wrapped = ring.place(400);

    EXPECT_EQ(wrapped.offset, 900u);
    EXPECT_EQ(wrapped.first_bytes, 100u);
    EXPECT_EQ(wrapped.second_bytes, 300u);
    EXPECT_FALSE(ring.has_room(301));
    EXPECT_TRUE(ring.has_room(300));
}

TEST(virtio_gpu_backend, a_send_ring_list_ending_at_the_end_leaves_the_next_at_the_start) {
    send_ring ring = {};
    ring.init(1000);

    ring.place(400);
    ring.free_oldest(400);
    ring_placement to_end = ring.place(600);

    EXPECT_EQ(to_end.first_bytes, 600u);
    EXPECT_EQ(to_end.second_bytes, 0u);
    EXPECT_FALSE(ring.wraps(400));

    ring_placement next = ring.place(400);
    EXPECT_EQ(next.offset, 0u);
    EXPECT_EQ(next.first_bytes, 400u);
}

TEST(virtio_gpu_backend, send_ring_room_returns_as_the_oldest_lists_free) {
    send_ring ring = {};
    ring.init(1000);

    ring.place(500);
    ring.place(500);
    EXPECT_FALSE(ring.has_room(1));

    ring.free_oldest(500);
    EXPECT_TRUE(ring.has_room(500));
    EXPECT_FALSE(ring.has_room(501));

    ring.free_oldest(500);
    EXPECT_TRUE(ring.has_room(1000));
}

// Three whole rings of a size that does not divide 2^32 carry an unreduced offset past 32 bits
static constexpr uint32_t ODD_RING_BYTES = 0x7FFFFFFF;

TEST(virtio_gpu_backend, send_ring_offsets_stay_in_the_ring_past_4_gib_of_lists) {
    send_ring ring = {};
    ring.init(ODD_RING_BYTES);

    for (uint32_t i = 0; i < 3; i++) {
        ring.place(ODD_RING_BYTES);
        ring.free_oldest(ODD_RING_BYTES);
    }

    EXPECT_EQ(ring.place(1).offset, 0u);
}

TEST(virtio_gpu_backend, a_send_ring_write_crossing_the_end_continues_at_the_start) {
    uint8_t ring[16] = {};
    ring_placement placement = {12, 4, 6};
    const uint8_t source[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    // The first write copies its 3 bytes only, short of the ring's end
    write_placed(ring, placement, 0, source, 3);
    EXPECT_EQ(ring[15], 0);

    write_placed(ring, placement, 3, source + 3, 7);
    const uint8_t expected[16] = {5, 6, 7, 8, 9, 10, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
    EXPECT_EQ(string::memcmp(ring, expected, sizeof(ring)), 0);
}

TEST(virtio_gpu_backend, a_send_ring_write_wholly_past_the_end_follows_the_bytes_wrapped_before_it) {
    uint8_t ring[16] = {};
    ring_placement placement = {12, 4, 8};
    const uint8_t source[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    write_placed(ring, placement, 0, source, 5);
    write_placed(ring, placement, 5, source + 5, 7);

    const uint8_t expected[16] = {5, 6, 7, 8, 9, 10, 11, 12, 0, 0, 0, 0, 1, 2, 3, 4};
    EXPECT_EQ(string::memcmp(ring, expected, sizeof(ring)), 0);
}

static virtio_gpu_submission returning_slots[GPU_QUEUE_PENDING_MAX];

TEST(virtio_gpu_backend, returned_lists_free_in_order_once_the_oldest_returns) {
    send_ring ring = {};
    ring.init(1000);
    for (uint64_t sequence = 1; sequence <= 3; sequence++) {
        returning_slots[sequence].ring_bytes = 300;
        returning_slots[sequence].is_answered_ok = true;
        ring.place(300);
    }

    returning_slots[2].has_returned = true;
    bool is_refused = true;
    EXPECT_EQ(free_returned_lists(returning_slots, ring, 0, 3, is_refused), static_cast<uint64_t>(0));
    EXPECT_FALSE(is_refused);
    EXPECT_FALSE(ring.has_room(101));

    returning_slots[1].has_returned = true;
    EXPECT_EQ(free_returned_lists(returning_slots, ring, 0, 3, is_refused), static_cast<uint64_t>(2));
    EXPECT_TRUE(ring.has_room(700));
    EXPECT_FALSE(ring.has_room(701));
    EXPECT_FALSE(returning_slots[2].has_returned);
}

static virtio_gpu_submission refused_slots[GPU_QUEUE_PENDING_MAX];

TEST(virtio_gpu_backend, returned_lists_from_a_refused_one_on_stay_unfinished) {
    send_ring ring = {};
    ring.init(1000);
    for (uint64_t sequence = 1; sequence <= 5; sequence++) {
        refused_slots[sequence].ring_bytes = 100;
        refused_slots[sequence].has_returned = true;
        refused_slots[sequence].is_answered_ok = sequence != 3;
        ring.place(100);
    }

    bool is_refused = false;
    EXPECT_EQ(free_returned_lists(refused_slots, ring, 0, 5, is_refused), static_cast<uint64_t>(2));
    EXPECT_TRUE(is_refused);
    EXPECT_TRUE(refused_slots[3].has_returned);

    is_refused = false;
    EXPECT_EQ(free_returned_lists(refused_slots, ring, 2, 5, is_refused), static_cast<uint64_t>(2));
    EXPECT_TRUE(is_refused);
    EXPECT_TRUE(ring.has_room(700));
    EXPECT_FALSE(ring.has_room(701));
}

static virtio_gpu_submission wrapping_slots[GPU_QUEUE_PENDING_MAX];

TEST(virtio_gpu_backend, returned_lists_finish_across_the_slots_wrap) {
    send_ring ring = {};
    ring.init(1000);
    for (uint64_t sequence = 63; sequence <= 66; sequence++) {
        virtio_gpu_submission& sub = wrapping_slots[sequence % GPU_QUEUE_PENDING_MAX];
        sub.ring_bytes = 100;
        sub.has_returned = true;
        sub.is_answered_ok = true;
        ring.place(100);
    }

    bool is_refused = true;
    EXPECT_EQ(free_returned_lists(wrapping_slots, ring, 62, 66, is_refused), static_cast<uint64_t>(66));
    EXPECT_FALSE(is_refused);
    EXPECT_TRUE(ring.has_room(1000));
}
