#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fake_backend.h"
#include "clock/clock.h"
#include "fs/fs.h"

TEST_SUITE(gpu_context);

using gpu_test::close_handle;
using gpu_test::FAKE_VA_START;
using gpu_test::session;
using test_helpers::spin_wait_ge;

constexpr uint64_t PAGE = pmm::PAGE_SIZE;
constexpr uint64_t PROT_READ_ARG = 0x1;
constexpr uint64_t PROT_READ_WRITE_ARG = 0x3;

// The last page of the 64-bit address space, where a range of two pages wraps
constexpr uint64_t TOP_PAGE = ~(PAGE - 1);

// A destroyed queue's index is free once its cleanup ran on the reaper, so a create retries while the slot is taken
static int64_t new_queue_once_free(session& s, int32_t context, uint32_t kind) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    int64_t rc = s.new_queue(context, kind);
    while (rc == syscall::EBUSY && clock::now_ns() < deadline) {
        rc = s.new_queue(context, kind);
    }

    return rc;
}

static gpu_test::fake_backend g_lifetime_fake;

// A context starts live with a space of its own, and its last handle's close destroys the space on the reaper
TEST(gpu_context, a_context_starts_live_and_its_last_close_destroys_its_space) {
    session s(g_lifetime_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    EXPECT_EQ(s.lost(context), static_cast<int64_t>(0));
    ASSERT_EQ(g_lifetime_fake.space_count, 1u);
    EXPECT_TRUE(g_lifetime_fake.spaces[0].core != nullptr);

    int32_t reader = s.restrict_to(context, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0);
    close_handle(context);
    EXPECT_EQ(s.lost(reader), static_cast<int64_t>(0));
    EXPECT_FALSE(g_lifetime_fake.spaces[0].destroyed);

    close_handle(reader);
    EXPECT_TRUE(spin_wait_ge(g_lifetime_fake.spaces_destroyed, 1));
}

static gpu_test::fake_backend g_create_fake;

// A create refuses a nonzero lost or fault_va and passes on the backend's refusal, a query needs RIGHT_READ, and each
// other call naming the context needs RIGHT_WRITE
TEST(gpu_context, create_refuses_a_nonzero_lost_or_fault_va_and_only_query_takes_a_read_only_handle) {
    session s(g_create_fake);
    ASSERT_TRUE(s.ready());

    gpu_context_args with_lost = {.context = -1, .lost = GPU_LOST_HUNG, .fault_va = 0};
    EXPECT_EQ(s.call(GPU_CONTEXT_CREATE, with_lost), syscall::EINVAL);
    gpu_context_args with_fault = {.context = -1, .lost = 0, .fault_va = PAGE};
    EXPECT_EQ(s.call(GPU_CONTEXT_CREATE, with_fault), syscall::EINVAL);

    g_create_fake.space_result = gpu::ERR_NO_MEMORY;
    EXPECT_EQ(s.new_context(), syscall::ENOMEM);
    g_create_fake.space_result = gpu::OK;
    EXPECT_EQ(g_create_fake.space_count, 0u);

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    int32_t writer = s.restrict_to(context, GPU_RIGHT_WRITE);
    ASSERT_TRUE(writer >= 0);
    EXPECT_EQ(s.lost(writer), syscall::EBADF);

    int32_t reader = s.restrict_to(context, GPU_RIGHT_READ);
    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(reader >= 0 && buffer >= 0);
    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));
    ASSERT_EQ(s.map(context, buffer, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.lost(reader), static_cast<int64_t>(0));

    EXPECT_EQ(s.new_queue(reader, GPU_QUEUE_GRAPHICS), syscall::EBADF);
    EXPECT_EQ(s.destroy_queue(reader, 0), syscall::EBADF);
    EXPECT_EQ(s.map(reader, buffer, FAKE_VA_START + PAGE, 0, PAGE, 0), syscall::EBADF);
    EXPECT_EQ(s.unmap(reader, FAKE_VA_START, PAGE), syscall::EBADF);

    EXPECT_EQ(g_create_fake.queue_count, 1u);
    EXPECT_EQ(g_create_fake.stops_asked.load_acquire(), 0u);
    EXPECT_EQ(g_create_fake.map_calls, 1u);
    EXPECT_EQ(g_create_fake.unmap_calls, 0u);

    close_handle(buffer);
    close_handle(reader);
    close_handle(writer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_create_fake.spaces_destroyed, 1));
}

static gpu_test::fake_backend g_queue_fake;

// A context holds up to GPU_QUEUES_MAX queues of the kinds its device offers, the queue calls refuse nonzero padding
// and a destroy's kind, and a destroyed queue's index is free again once its cleanup ran
TEST(gpu_context, queues_take_offered_kinds_up_to_the_most_and_free_their_index) {
    g_queue_fake.queue_kinds = 1u << GPU_QUEUE_GRAPHICS;
    session s(g_queue_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    EXPECT_EQ(s.new_queue(context, GPU_QUEUE_COPY), syscall::EINVAL);
    EXPECT_EQ(s.new_queue(context, 0), syscall::EINVAL);

    gpu_queue_args padded = {.context = context, .kind = GPU_QUEUE_GRAPHICS, .queue = ~0u, .padding = 1};
    EXPECT_EQ(s.call(GPU_QUEUE_CREATE, padded), syscall::EINVAL);
    EXPECT_EQ(g_queue_fake.queue_count, 0u);

    for (uint32_t i = 0; i < GPU_QUEUES_MAX; i++) {
        EXPECT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(i));
    }

    EXPECT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), syscall::EBUSY);

    gpu_queue_args with_kind = {.context = context, .kind = GPU_QUEUE_GRAPHICS, .queue = 3, .padding = 0};
    EXPECT_EQ(s.call(GPU_QUEUE_DESTROY, with_kind), syscall::EINVAL);
    gpu_queue_args with_padding = {.context = context, .kind = 0, .queue = 3, .padding = 1};
    EXPECT_EQ(s.call(GPU_QUEUE_DESTROY, with_padding), syscall::EINVAL);

    EXPECT_EQ(s.destroy_queue(context, 3), static_cast<int64_t>(0));
    EXPECT_EQ(s.destroy_queue(context, 3), syscall::EINVAL);
    EXPECT_EQ(s.destroy_queue(context, GPU_QUEUES_MAX), syscall::EINVAL);
    EXPECT_EQ(new_queue_once_free(s, context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(3));

    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_queue_fake.spaces_destroyed, 1));
    EXPECT_EQ(g_queue_fake.queues_destroyed.load_acquire(), GPU_QUEUES_MAX + 1);
}

static gpu_test::fake_backend g_write_back_fake;

// With the argument's page read-only, a context create destroys the space it made, and a queue create stops and
// destroys the queue it made
TEST(gpu_context, a_create_that_cannot_write_back_destroys_what_it_made) {
    session s(g_write_back_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);

    ASSERT_EQ(s.protect_page(PROT_READ_ARG), static_cast<int64_t>(0));
    EXPECT_EQ(s.new_context(), syscall::EINVAL);
    EXPECT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), syscall::EINVAL);
    ASSERT_EQ(s.protect_page(PROT_READ_WRITE_ARG), static_cast<int64_t>(0));

    EXPECT_EQ(g_write_back_fake.stops_asked.load_acquire(), 1u);
    EXPECT_TRUE(spin_wait_ge(g_write_back_fake.queues_destroyed, 1));
    EXPECT_TRUE(spin_wait_ge(g_write_back_fake.spaces_destroyed, 1));

    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_write_back_fake.spaces_destroyed, 2));
}

static gpu_test::fake_backend g_pending_stop_fake;

// A destroyed queue keeps its index and its context until the backend reports its stop, and a closed context
// destroys its space and drops its mappings' buffers only once every queue stopped
TEST(gpu_context, a_destroyed_queue_holds_its_index_and_context_until_the_backend_reports_its_stop) {
    g_pending_stop_fake.stops_on_reaper = false;
    session s(g_pending_stop_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    int32_t mapped = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(mapped >= 0);
    gpu::memory* mapped_memory = g_pending_stop_fake.last_created;

    ASSERT_EQ(s.map(context, mapped, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    close_handle(mapped);

    int32_t first_probe = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    int32_t second_probe = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    int32_t third_probe = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(first_probe >= 0 && second_probe >= 0 && third_probe >= 0);
    gpu::memory* third_memory = g_pending_stop_fake.last_created;

    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));
    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_COPY), static_cast<int64_t>(1));
    ASSERT_EQ(s.destroy_queue(context, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.destroy_queue(context, 0), syscall::EINVAL);
    EXPECT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(2));
    EXPECT_EQ(g_pending_stop_fake.stops_asked.load_acquire(), 1u);

    // A probe's release runs on the reaper after anything deferred before its close
    close_handle(context);
    close_handle(first_probe);
    EXPECT_TRUE(spin_wait_ge(g_pending_stop_fake.released_count, 1));
    EXPECT_EQ(g_pending_stop_fake.stops_asked.load_acquire(), 1u);

    gpu::queue_stopped(g_pending_stop_fake.queues[0].core);
    EXPECT_TRUE(spin_wait_ge(g_pending_stop_fake.stops_asked, 3));
    EXPECT_EQ(g_pending_stop_fake.queues_destroyed.load_acquire(), 1u);

    gpu::queue_stopped(g_pending_stop_fake.queues[1].core);
    close_handle(second_probe);
    EXPECT_TRUE(spin_wait_ge(g_pending_stop_fake.released_count, 2));

    // The second probe's release follows the first stage, so the third's follows anything that stage deferred
    close_handle(third_probe);
    EXPECT_TRUE(spin_wait_ge(g_pending_stop_fake.released_count, 3));
    EXPECT_TRUE(g_pending_stop_fake.last_released == third_memory);
    EXPECT_EQ(g_pending_stop_fake.released_count.load_acquire(), 3u);
    EXPECT_EQ(g_pending_stop_fake.spaces_destroyed.load_acquire(), 0u);

    gpu::queue_stopped(g_pending_stop_fake.queues[2].core);
    EXPECT_TRUE(spin_wait_ge(g_pending_stop_fake.released_count, 4));
    EXPECT_TRUE(g_pending_stop_fake.last_released == mapped_memory);
    EXPECT_EQ(g_pending_stop_fake.spaces_destroyed.load_acquire(), 1u);
    EXPECT_EQ(g_pending_stop_fake.queues_destroyed.load_acquire(), 3u);
}

static gpu_test::fake_backend g_close_order_fake;

// A mapping holds its buffer until the context's teardown, whichever handle closes first, and the teardown stops and
// destroys the queues the context still holds
TEST(gpu_context, a_mapping_holds_its_buffer_until_the_context_teardown_whichever_closes_first) {
    session s(g_close_order_fake);
    ASSERT_TRUE(s.ready());

    int32_t first = s.new_context();
    ASSERT_TRUE(first >= 0);
    int32_t first_buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(first_buffer >= 0);
    ASSERT_EQ(s.map(first, first_buffer, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    ASSERT_EQ(s.new_queue(first, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));

    // The queue's cleanup runs on the reaper after anything the buffer's close deferred
    close_handle(first_buffer);
    ASSERT_EQ(s.destroy_queue(first, 0), static_cast<int64_t>(0));
    EXPECT_TRUE(spin_wait_ge(g_close_order_fake.queues_destroyed, 1));
    EXPECT_EQ(g_close_order_fake.released_count.load_acquire(), 0u);

    close_handle(first);
    EXPECT_TRUE(spin_wait_ge(g_close_order_fake.released_count, 1));
    EXPECT_EQ(g_close_order_fake.spaces_destroyed.load_acquire(), 1u);

    int32_t second = s.new_context();
    ASSERT_TRUE(second >= 0);
    int32_t second_buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(second_buffer >= 0);
    ASSERT_EQ(s.map(second, second_buffer, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    ASSERT_EQ(s.new_queue(second, GPU_QUEUE_COPY), static_cast<int64_t>(0));

    close_handle(second);
    EXPECT_TRUE(spin_wait_ge(g_close_order_fake.spaces_destroyed, 2));
    EXPECT_EQ(g_close_order_fake.stops_asked.load_acquire(), 2u);
    EXPECT_EQ(g_close_order_fake.queues_destroyed.load_acquire(), 2u);
    EXPECT_EQ(g_close_order_fake.released_count.load_acquire(), 1u);

    close_handle(second_buffer);
    EXPECT_TRUE(spin_wait_ge(g_close_order_fake.released_count, 2));
}

static gpu_test::fake_backend g_no_memory_fake;

// An object the core cannot allocate fails its call with ENOMEM before the backend is asked, changing nothing
TEST(gpu_context, a_failed_allocation_returns_enomem_and_changes_nothing) {
    session s(g_no_memory_fake);
    ASSERT_TRUE(s.ready());

    gpu::__dbg_test_fail_next_allocation();
    EXPECT_EQ(s.new_context(), syscall::ENOMEM);
    EXPECT_EQ(g_no_memory_fake.space_count, 0u);

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    gpu::__dbg_test_fail_next_allocation();
    EXPECT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), syscall::ENOMEM);
    EXPECT_EQ(g_no_memory_fake.queue_count, 0u);
    EXPECT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));

    int32_t buffer = s.new_buffer(4 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);
    gpu::__dbg_test_fail_next_allocation();
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, 0, 4 * PAGE, 0), syscall::ENOMEM);
    EXPECT_EQ(g_no_memory_fake.map_calls, 0u);
    ASSERT_EQ(s.map(context, buffer, FAKE_VA_START, 0, 4 * PAGE, 0), static_cast<int64_t>(0));

    gpu::__dbg_test_fail_next_allocation();
    EXPECT_EQ(s.unmap(context, FAKE_VA_START + PAGE, PAGE), syscall::ENOMEM);
    EXPECT_EQ(g_no_memory_fake.unmap_calls, 0u);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START + PAGE, 0, PAGE, 0), syscall::EBUSY);
    EXPECT_EQ(s.unmap(context, FAKE_VA_START + PAGE, PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(g_no_memory_fake.spaces[0].range_count, 2u);

    close_handle(buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_no_memory_fake.released_count, 1));
}

static gpu_test::fake_backend g_map_fake;

// Mappings never overlap, an unmap inside one splits it and one at either end keeps the rest, and the range freed
// takes another mapping
TEST(gpu_context, map_refuses_an_overlap_and_unmap_splits_a_mapping) {
    session s(g_map_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    int32_t big = s.new_buffer(4 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(big >= 0);
    int32_t small = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(small >= 0);

    EXPECT_EQ(s.map(context, big, FAKE_VA_START, 0, 4 * PAGE, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + 3 * PAGE, 0, PAGE, 0), syscall::EBUSY);

    EXPECT_EQ(s.unmap(context, FAKE_VA_START + PAGE, 2 * PAGE), static_cast<int64_t>(0));
    gpu_test::fake_space& space = g_map_fake.spaces[0];
    EXPECT_EQ(space.range_count, 2u);
    EXPECT_FALSE(space.bad_call);

    EXPECT_EQ(s.map(context, big, FAKE_VA_START + 2 * PAGE, 0, 2 * PAGE, 0), syscall::EBUSY);
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + PAGE, 0, PAGE, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + 2 * PAGE, 0, PAGE, GPU_MAP_READ_ONLY), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + 3 * PAGE, 0, PAGE, 0), syscall::EBUSY);
    EXPECT_FALSE(space.bad_call);

    EXPECT_EQ(s.unmap(context, FAKE_VA_START, 4 * PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(space.range_count, 0u);
    EXPECT_EQ(g_map_fake.last_unmap.va, FAKE_VA_START);
    EXPECT_EQ(g_map_fake.last_unmap.size, 4 * PAGE);
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, 4 * PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(g_map_fake.unmap_calls, 2u);

    int32_t ends = s.new_buffer(3 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(ends >= 0);
    ASSERT_EQ(s.map(context, ends, FAKE_VA_START, 0, 3 * PAGE, 0), static_cast<int64_t>(0));
    close_handle(ends);

    EXPECT_EQ(s.unmap(context, FAKE_VA_START, PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + 3 * PAGE, 0, PAGE, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + 2 * PAGE, 0, PAGE, 0), syscall::EBUSY);

    EXPECT_EQ(s.unmap(context, FAKE_VA_START + 2 * PAGE, PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + 2 * PAGE, 0, PAGE, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, small, FAKE_VA_START + PAGE, 0, PAGE, 0), syscall::EBUSY);
    EXPECT_EQ(g_map_fake.released_count.load_acquire(), 0u);
    EXPECT_FALSE(space.bad_call);

    EXPECT_EQ(s.unmap(context, FAKE_VA_START + PAGE, PAGE), static_cast<int64_t>(0));
    EXPECT_TRUE(spin_wait_ge(g_map_fake.released_count, 1));

    close_handle(small);
    close_handle(big);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_map_fake.released_count, 3));
}

static gpu_test::fake_backend g_map_call_fake;

// Map hands the backend the buffer's memory with the range, offset, kind and protection the call names
TEST(gpu_context, map_hands_the_backend_the_memory_range_kind_and_protection) {
    session s(g_map_call_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    int32_t buffer = s.new_buffer(4 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);
    gpu::memory* memory = g_map_call_fake.last_created;

    gpu_map_args args = {.context = context, .buffer = buffer, .va = FAKE_VA_START + PAGE, .offset = 2 * PAGE,
                         .size = PAGE, .kind = 7, .flags = GPU_MAP_READ_ONLY};
    EXPECT_EQ(s.call(GPU_MAP, args), static_cast<int64_t>(0));
    const gpu_test::fake_map_call& call = g_map_call_fake.last_map;

    EXPECT_EQ(call.mem, memory);
    EXPECT_EQ(call.va, FAKE_VA_START + PAGE);
    EXPECT_EQ(call.offset, 2 * PAGE);
    EXPECT_EQ(call.size, PAGE);
    EXPECT_EQ(call.kind, 7u);
    EXPECT_TRUE(call.read_only);

    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START + 3 * PAGE, PAGE, 3 * PAGE, 0), static_cast<int64_t>(0));
    EXPECT_EQ(call.va, FAKE_VA_START + 3 * PAGE);
    EXPECT_EQ(call.offset, PAGE);
    EXPECT_EQ(call.size, 3 * PAGE);
    EXPECT_EQ(call.kind, 0u);
    EXPECT_FALSE(call.read_only);

    close_handle(buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_map_call_fake.released_count, 1));
}

static gpu_test::fake_backend g_map_arguments_fake;

// Map refuses a range outside the user range or the buffer, an unaligned or empty one, an unknown flag and a buffer
// handle without write unless read only, and unmap an unaligned, empty or wrapping range
TEST(gpu_context, map_and_unmap_refuse_each_invalid_argument) {
    session s(g_map_arguments_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    int32_t buffer = s.new_buffer(2 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);
    int32_t reader = s.restrict_to(buffer, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0);

    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START - PAGE, 0, PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, gpu_test::FAKE_VA_END - PAGE, 0, 2 * PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, PAGE, 2 * PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START + 1, 0, PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, 0, 0, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, 0, PAGE, 0x2u), syscall::EINVAL);

    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, 1, PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, 0, PAGE + 1, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, TOP_PAGE, 0, 2 * PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, TOP_PAGE, 2 * PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(g_map_arguments_fake.map_calls, 0u);

    EXPECT_EQ(s.map(context, reader, FAKE_VA_START, 0, PAGE, 0), syscall::EBADF);
    EXPECT_EQ(s.map(context, reader, FAKE_VA_START, 0, PAGE, GPU_MAP_READ_ONLY), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(buffer, buffer, FAKE_VA_START + PAGE, 0, PAGE, 0), syscall::EBADF);

    g_map_arguments_fake.map_result = gpu::ERR_REFUSED;
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START + PAGE, 0, PAGE, 0), syscall::EINVAL);
    g_map_arguments_fake.map_result = gpu::ERR_NO_MEMORY;
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START + PAGE, 0, PAGE, 0), syscall::ENOMEM);
    g_map_arguments_fake.map_result = gpu::OK;
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START + PAGE, 0, PAGE, 0), static_cast<int64_t>(0));

    EXPECT_EQ(s.unmap(context, FAKE_VA_START + 1, PAGE), syscall::EINVAL);
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, 1), syscall::EINVAL);
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, 0), syscall::EINVAL);
    EXPECT_EQ(s.unmap(context, TOP_PAGE, 2 * PAGE), syscall::EINVAL);
    EXPECT_EQ(g_map_arguments_fake.unmap_calls, 0u);

    close_handle(reader);
    close_handle(buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_map_arguments_fake.released_count, 1));
}

static gpu_test::fake_backend g_by_id_fake;

// Without a user range a mapping is named by its buffer's device_id and maps the whole buffer, its unmap releases a
// buffer no handle holds, and an unmap naming an id no mapping holds is refused
TEST(gpu_context, without_a_user_range_a_buffer_maps_whole_by_its_device_id) {
    g_by_id_fake.user_range = false;
    session s(g_by_id_fake);
    ASSERT_TRUE(s.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    gpu_buffer_args args = gpu_test::buffer_request(2 * PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_EQ(s.call(GPU_BUFFER_CREATE, args), static_cast<int64_t>(0));

    gpu_map_args with_kind = {.context = context, .buffer = args.buffer, .va = 0, .offset = 0, .size = 0, .kind = 7,
                              .flags = 0};
    EXPECT_EQ(s.call(GPU_MAP, with_kind), syscall::EINVAL);
    EXPECT_EQ(s.map(context, args.buffer, PAGE, 0, 0, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, args.buffer, 0, 0, 0, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, args.buffer, 0, 0, 0, 0), syscall::EBUSY);

    EXPECT_EQ(s.unmap(context, args.device_id + 1, 0), syscall::EINVAL);
    EXPECT_EQ(s.unmap(context, args.device_id, 0), static_cast<int64_t>(0));
    EXPECT_EQ(g_by_id_fake.last_unmap.va, static_cast<uint64_t>(args.device_id));
    EXPECT_EQ(g_by_id_fake.last_unmap.size, 2 * PAGE);
    EXPECT_EQ(s.unmap(context, args.device_id, 0), syscall::EINVAL);

    gpu_buffer_args second = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_EQ(s.call(GPU_BUFFER_CREATE, second), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, second.buffer, 0, 0, PAGE, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, second.buffer, 0, PAGE, 0, 0), syscall::EINVAL);
    EXPECT_EQ(s.map(context, args.buffer, 0, 0, 0, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.map(context, second.buffer, 0, 0, 0, 0), static_cast<int64_t>(0));

    close_handle(second.buffer);
    EXPECT_EQ(s.unmap(context, second.device_id, PAGE), static_cast<int64_t>(0));
    EXPECT_EQ(g_by_id_fake.last_unmap.va, static_cast<uint64_t>(second.device_id));
    EXPECT_EQ(g_by_id_fake.last_unmap.size, PAGE);
    EXPECT_TRUE(spin_wait_ge(g_by_id_fake.released_count, 1));
    EXPECT_TRUE(g_by_id_fake.last_released == g_by_id_fake.last_created);

    close_handle(args.buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_by_id_fake.released_count, 2));
}

static gpu_test::fake_backend g_context_memory_fake;
static gpu_test::fake_backend g_no_context_placement_fake;

// A GPU_PLACEMENT_CONTEXT buffer passes the context's space and the backend's id to the backend, and only that
// placement takes a context or an id, on a device that offers it
TEST(gpu_context, a_context_placement_buffer_names_the_space_and_the_backend_id) {
    g_context_memory_fake.placements = gpu_test::ALL_PLACEMENTS;
    session s(g_context_memory_fake);
    ASSERT_TRUE(s.ready());
    session without(g_no_context_placement_fake);
    ASSERT_TRUE(without.ready());

    int32_t its_context = without.new_context();
    ASSERT_TRUE(its_context >= 0);
    gpu_buffer_args absent = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_CONTEXT, 0);
    absent.context = its_context;
    EXPECT_EQ(without.call(GPU_BUFFER_CREATE, absent), syscall::EINVAL);
    EXPECT_EQ(g_no_context_placement_fake.create_calls, 0u);

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);

    gpu_buffer_args args = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_CONTEXT, 0);
    args.context = context;
    args.backend_id = 77;
    ASSERT_EQ(s.call(GPU_BUFFER_CREATE, args), static_cast<int64_t>(0));
    EXPECT_TRUE(g_context_memory_fake.last_create_space == &g_context_memory_fake.spaces[0]);
    EXPECT_EQ(g_context_memory_fake.last_backend_id, static_cast<uint64_t>(77));

    gpu_buffer_args elsewhere = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    elsewhere.backend_id = 77;
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, elsewhere), syscall::EINVAL);
    elsewhere.backend_id = 0;
    elsewhere.context = context;
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, elsewhere), syscall::EINVAL);

    int32_t reader = s.restrict_to(context, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0);
    gpu_buffer_args by_reader = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_CONTEXT, 0);
    by_reader.context = reader;
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, by_reader), syscall::EBADF);

    close_handle(its_context);
    close_handle(reader);
    close_handle(args.buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_context_memory_fake.released_count, 1));
}

static gpu_test::fake_backend g_device_check_fake;
static gpu_test::fake_backend g_other_device_fake;

// A context or buffer of another device is refused by query, queue create, restrict, map and a context placement
// create, before either backend is asked
TEST(gpu_context, a_context_or_buffer_of_another_device_is_refused) {
    g_other_device_fake.placements = gpu_test::ALL_PLACEMENTS;
    session s(g_device_check_fake);
    ASSERT_TRUE(s.ready());
    session other(g_other_device_fake);
    ASSERT_TRUE(other.ready());

    int32_t context = s.new_context();
    ASSERT_TRUE(context >= 0);
    int32_t buffer = other.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(buffer >= 0);

    EXPECT_EQ(other.lost(context), syscall::EINVAL);
    EXPECT_EQ(other.new_queue(context, GPU_QUEUE_GRAPHICS), syscall::EINVAL);
    EXPECT_EQ(other.restrict_to(context, GPU_RIGHT_READ), syscall::EINVAL);
    EXPECT_EQ(s.map(context, buffer, FAKE_VA_START, 0, PAGE, 0), syscall::EINVAL);

    gpu_buffer_args theirs = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_CONTEXT, 0);
    theirs.context = context;
    EXPECT_EQ(other.call(GPU_BUFFER_CREATE, theirs), syscall::EINVAL);
    EXPECT_EQ(g_other_device_fake.create_calls, 1u);
    EXPECT_EQ(g_other_device_fake.queue_count, 0u);
    EXPECT_EQ(g_device_check_fake.map_calls + g_other_device_fake.map_calls, 0u);

    close_handle(buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_device_check_fake.spaces_destroyed, 1));
}
