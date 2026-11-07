#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fake_backend.h"
#include "fs/fs.h"
#include "mm/paging.h"
#include "syscall/handlers/sys_mmap.h"

TEST_SUITE(gpu_buffer);

using gpu_test::close_handle;
using gpu_test::drain_reaper;
using gpu_test::session;
using test_helpers::spin_wait_ge;
using test_helpers::user_space_scope;

constexpr uint64_t PROT_READ_ARG = 0x1;
constexpr uint64_t PROT_READ_WRITE_ARG = 0x3;
constexpr uint64_t MAP_SHARED_ARG = 0x01;
constexpr uint64_t PAGE = pmm::PAGE_SIZE;
constexpr uint32_t UNKNOWN_BUFFER_FLAG = 0x4u;
constexpr uint32_t EVERY_PLACEMENT = ~0u;
constexpr uint32_t UNKNOWN_PLACEMENTS[] = {0, 4, 32};

// Maps `length` bytes of the buffer `handle` names from `offset`, shared, returning the address or a syscall code
static int64_t map_buffer(session& s, int32_t handle, uint64_t length, uint64_t prot, uint64_t offset = 0) {
    user_space_scope scope(s.page.ctx);
    return sys_mmap(0, length, prot, MAP_SHARED_ARG, static_cast<uint64_t>(handle), offset);
}

static int64_t unmap(session& s, int64_t addr, uint64_t length) {
    user_space_scope scope(s.page.ctx);
    return sys_munmap(static_cast<uint64_t>(addr), length, 0, 0, 0, 0);
}

static pmm::phys_addr_t phys_at(session& s, int64_t addr) {
    return paging::get_physical(static_cast<uintptr_t>(addr), s.page.ctx->pt_root);
}

static gpu_test::fake_backend g_created_fake;

// The backend gets each request rounded up to whole pages and names each buffer, and the handle holds both rights
TEST(gpu_buffer, create_gives_the_backend_the_rounded_request_and_returns_its_id_and_a_handle_with_both_rights) {
    session s(g_created_fake);
    ASSERT_TRUE(s.ready());

    gpu_buffer_args system = gpu_test::buffer_request(1, GPU_PLACEMENT_SYSTEM, 0);
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, system), static_cast<int64_t>(0));
    ASSERT_NOT_NULL(g_created_fake.last_created);
    EXPECT_EQ(g_created_fake.last_created->size, PAGE);
    EXPECT_EQ(g_created_fake.last_created->placement, static_cast<uint32_t>(GPU_PLACEMENT_SYSTEM));

    uint32_t scanout_flags = GPU_BUFFER_SCANOUT | GPU_BUFFER_MAPPABLE;
    gpu_buffer_args scanout = gpu_test::buffer_request(PAGE + 1, GPU_PLACEMENT_VIDEO, scanout_flags);
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, scanout), static_cast<int64_t>(0));
    EXPECT_EQ(g_created_fake.last_created->size, 2 * PAGE);
    EXPECT_EQ(g_created_fake.last_created->placement, static_cast<uint32_t>(GPU_PLACEMENT_VIDEO));
    EXPECT_EQ(g_created_fake.last_created->flags, scanout_flags);

    EXPECT_EQ(system.device_id, 1u);
    EXPECT_EQ(scanout.device_id, 2u);

    int32_t both = s.restrict_to(scanout.buffer, GPU_RIGHT_READ | GPU_RIGHT_WRITE);
    EXPECT_TRUE(both >= 0);

    close_handle(both);
    close_handle(scanout.buffer);
    close_handle(system.buffer);
}

static gpu_test::fake_backend g_refused_fake;
static gpu_test::fake_backend g_every_placement_fake;

// Each invalid request is refused with EINVAL before any backend call, including VIDEO on a device without it, a
// scanout buffer outside video memory and a placement the core does not know even when `placements` sets every bit
TEST(gpu_buffer, create_refuses_each_invalid_argument_before_the_backend) {
    g_refused_fake.placements = gpu_test::SYSTEM_ONLY;
    g_every_placement_fake.placements = EVERY_PLACEMENT;
    session s(g_refused_fake);
    session every(g_every_placement_fake);
    ASSERT_TRUE(s.ready());
    ASSERT_TRUE(every.ready());

    constexpr size_t CASES = 8;
    gpu_buffer_args refused[CASES] = {};
    for (gpu_buffer_args& args : refused) {
        args = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    }

    refused[0].size = 0;
    refused[1].size = ~static_cast<uint64_t>(0);

    refused[2].placement = GPU_PLACEMENT_VIDEO;
    refused[3].flags = UNKNOWN_BUFFER_FLAG;
    refused[4].flags = GPU_BUFFER_SCANOUT;

    refused[5].source = 1;
    refused[6].context = 1;
    refused[7].backend_id = 1;

    for (gpu_buffer_args& args : refused) {
        EXPECT_EQ(s.call(GPU_BUFFER_CREATE, args), syscall::EINVAL);
        EXPECT_EQ(args.buffer, -1);
    }

    for (uint32_t placement : UNKNOWN_PLACEMENTS) {
        gpu_buffer_args args = gpu_test::buffer_request(PAGE, placement, 0);
        EXPECT_EQ(every.call(GPU_BUFFER_CREATE, args), syscall::EINVAL);
    }

    EXPECT_EQ(g_refused_fake.create_calls, 0u);
    EXPECT_EQ(g_every_placement_fake.create_calls, 0u);
}

static gpu_test::fake_backend g_failing_fake;

// The backend's failures return ENOMEM and EIO through the syscall, with no handle written
TEST(gpu_buffer, create_returns_the_backends_failure_with_no_handle) {
    session s(g_failing_fake);
    ASSERT_TRUE(s.ready());

    gpu_buffer_args args = gpu_test::buffer_request(PAGE, GPU_PLACEMENT_VIDEO, 0);
    g_failing_fake.create_result = gpu::ERR_NO_MEMORY;
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, args), syscall::ENOMEM);
    EXPECT_EQ(args.buffer, -1);

    g_failing_fake.create_result = gpu::ERR_DEVICE;
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, args), syscall::EIO);
    EXPECT_EQ(args.buffer, -1);
    EXPECT_EQ(g_failing_fake.create_calls, 2u);
}

static gpu_test::fake_backend g_released_fake;

// The reaper returns the memory to the backend once no handle and no mapping refer to the buffer, whichever of the two
// ends last, a restricted handle included
TEST(gpu_buffer, the_reaper_returns_the_memory_once_no_handle_or_mapping_refers_to_the_buffer) {
    session s(g_released_fake);
    ASSERT_TRUE(s.ready());

    int32_t first = s.new_buffer(PAGE, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE);
    ASSERT_TRUE(first >= 0);
    gpu::memory* first_memory = g_released_fake.last_created;

    int32_t reader = s.restrict_to(first, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0);
    int64_t addr = map_buffer(s, first, PAGE, PROT_READ_WRITE_ARG);
    ASSERT_TRUE(addr > 0);

    close_handle(first);
    EXPECT_EQ(phys_at(s, addr), g_released_fake.last_created->phys);
    EXPECT_EQ(unmap(s, addr, PAGE), static_cast<int64_t>(0));
    EXPECT_TRUE(drain_reaper(s, g_released_fake, 0));

    close_handle(reader);
    ASSERT_TRUE(spin_wait_ge(g_released_fake.released_count, 2));
    EXPECT_EQ(g_released_fake.last_released, first_memory);

    int32_t second = s.new_buffer(PAGE, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE);
    ASSERT_TRUE(second >= 0);
    addr = map_buffer(s, second, PAGE, PROT_READ_ARG);
    ASSERT_TRUE(addr > 0);

    close_handle(second);
    EXPECT_TRUE(drain_reaper(s, g_released_fake, 2));
    EXPECT_EQ(unmap(s, addr, PAGE), static_cast<int64_t>(0));
    ASSERT_TRUE(spin_wait_ge(g_released_fake.released_count, 4));
}

static gpu_test::fake_backend g_mapped_fake;

// A shared mapping reaches the backend's pages with the cache type its view asks for
TEST(gpu_buffer, mmap_maps_the_backends_pages_with_its_cache_type) {
    session s(g_mapped_fake);
    ASSERT_TRUE(s.ready());

    int32_t cached = s.new_buffer(2 * PAGE, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE);
    ASSERT_TRUE(cached >= 0);
    pmm::phys_addr_t cached_phys = g_mapped_fake.last_created->phys;

    int64_t addr = map_buffer(s, cached, 2 * PAGE, PROT_READ_WRITE_ARG);
    ASSERT_TRUE(addr > 0);
    EXPECT_EQ(phys_at(s, addr), cached_phys);
    EXPECT_EQ(phys_at(s, addr + PAGE), cached_phys + PAGE);
    paging::page_flags_t flags = paging::get_page_flags(static_cast<uintptr_t>(addr), s.page.ctx->pt_root);
    EXPECT_EQ(flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);

    g_mapped_fake.cacheable = false;
    int32_t combined = s.new_buffer(2 * PAGE, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE);
    ASSERT_TRUE(combined >= 0);
    pmm::phys_addr_t combined_phys = g_mapped_fake.last_created->phys;

    int64_t second = map_buffer(s, combined, PAGE, PROT_READ_ARG, PAGE);
    ASSERT_TRUE(second > 0);
    EXPECT_EQ(phys_at(s, second), combined_phys + PAGE);
    flags = paging::get_page_flags(static_cast<uintptr_t>(second), s.page.ctx->pt_root);
    EXPECT_EQ(flags & paging::PAGE_TYPE_MASK, paging::PAGE_WC);

    close_handle(combined);
    close_handle(cached);
}

static gpu_test::fake_backend g_unmappable_fake;

// A buffer without GPU_BUFFER_MAPPABLE, a range past the buffer's end or its view's and a misaligned or refused view
// map nothing, and through a handle without RIGHT_WRITE a mapping is never writable, at mmap or after mprotect
TEST(gpu_buffer, mmap_refuses_what_it_cannot_map_and_never_maps_writable_without_the_write_right) {
    session s(g_unmappable_fake);
    ASSERT_TRUE(s.ready());

    int32_t unmappable = s.new_buffer(PAGE, GPU_PLACEMENT_VIDEO, 0);
    ASSERT_TRUE(unmappable >= 0);
    EXPECT_EQ(map_buffer(s, unmappable, PAGE, PROT_READ_ARG), syscall::EINVAL);

    int32_t mappable = s.new_buffer(PAGE, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE);
    ASSERT_TRUE(mappable >= 0);
    EXPECT_EQ(map_buffer(s, mappable, 2 * PAGE, PROT_READ_ARG), syscall::EINVAL);
    EXPECT_EQ(map_buffer(s, mappable, PAGE, PROT_READ_ARG, PAGE), syscall::EINVAL);

    int32_t reader = s.restrict_to(mappable, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0);
    EXPECT_EQ(map_buffer(s, reader, PAGE, PROT_READ_WRITE_ARG), syscall::EACCES);
    int64_t addr = map_buffer(s, reader, PAGE, PROT_READ_ARG);
    ASSERT_TRUE(addr > 0);
    {
        user_space_scope scope(s.page.ctx);
        EXPECT_EQ(sys_mprotect(static_cast<uint64_t>(addr), PAGE, PROT_READ_WRITE_ARG, 0, 0, 0), syscall::EACCES);
    }

    g_unmappable_fake.view_size = PAGE;
    int32_t short_view = s.new_buffer(2 * PAGE, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE);
    ASSERT_TRUE(short_view >= 0);
    EXPECT_EQ(map_buffer(s, short_view, PAGE, PROT_READ_ARG, PAGE), syscall::ENOMEM);

    g_unmappable_fake.view_size = 0;
    g_unmappable_fake.view_shift = PAGE / 2;
    EXPECT_EQ(map_buffer(s, mappable, PAGE, PROT_READ_ARG), syscall::ENOMEM);

    g_unmappable_fake.view_shift = 0;
    g_unmappable_fake.view_result = gpu::ERR_DEVICE;
    EXPECT_EQ(map_buffer(s, mappable, PAGE, PROT_READ_ARG), syscall::ENOMEM);
    EXPECT_EQ(map_buffer(s, mappable, ~static_cast<uint64_t>(0), PROT_READ_ARG), syscall::EINVAL);

    close_handle(short_view);
    close_handle(reader);
    close_handle(mappable);
    close_handle(unmappable);
}

// Returns `gpu::lookup_buffer()`'s code for `handle` and `rights`, dropping the reference a lookup that succeeds takes
static int32_t look_up_and_release(int32_t handle, uint32_t rights) {
    gpu::buffer* found = nullptr;
    gpu::memory_view view = {};
    int32_t rc = fs::ERR_IO;
    RUN_ELEVATED(rc = gpu::lookup_buffer(handle, rights, &found, view));
    if (rc == fs::OK) {
        RUN_ELEVATED(gpu::release_buffer(found));
    }

    return rc;
}

static gpu_test::fake_backend g_looked_up_fake;

// `lookup_buffer()` describes a buffer's memory and holds the buffer past its handle's close until `release_buffer()`
TEST(gpu_buffer, lookup_holds_the_buffer_until_its_release) {
    session s(g_looked_up_fake);
    ASSERT_TRUE(s.ready());

    int32_t handle = s.new_buffer(2 * PAGE, GPU_PLACEMENT_VIDEO, GPU_BUFFER_SCANOUT);
    ASSERT_TRUE(handle >= 0);
    gpu::memory* memory = g_looked_up_fake.last_created;

    gpu::buffer* found = nullptr;
    gpu::memory_view view = {};
    int32_t rc = fs::ERR_IO;
    RUN_ELEVATED(rc = gpu::lookup_buffer(handle, GPU_RIGHT_READ, &found, view));
    ASSERT_EQ(rc, fs::OK);
    EXPECT_NOT_NULL(found);

    EXPECT_EQ(view.owner, static_cast<gpu::backend*>(&g_looked_up_fake));
    EXPECT_EQ(view.mem, memory);
    EXPECT_EQ(view.size, 2 * PAGE);
    EXPECT_EQ(view.placement, static_cast<uint32_t>(GPU_PLACEMENT_VIDEO));
    EXPECT_EQ(view.flags, static_cast<uint32_t>(GPU_BUFFER_SCANOUT));

    close_handle(handle);
    EXPECT_TRUE(drain_reaper(s, g_looked_up_fake, 0));

    RUN_ELEVATED(gpu::release_buffer(found));
    ASSERT_TRUE(spin_wait_ge(g_looked_up_fake.released_count, 2));
    EXPECT_EQ(g_looked_up_fake.last_released, memory);
}

static gpu_test::fake_backend g_refused_lookup_fake;

// `lookup_buffer()` refuses a handle naming no buffer and one lacking the rights asked, holding no reference
TEST(gpu_buffer, lookup_refuses_a_handle_naming_no_buffer_or_lacking_the_rights) {
    session s(g_refused_lookup_fake);
    ASSERT_TRUE(s.ready());

    int32_t handle = s.new_buffer(PAGE, GPU_PLACEMENT_VIDEO, 0);
    ASSERT_TRUE(handle >= 0);
    int32_t writer = s.restrict_to(handle, GPU_RIGHT_WRITE);
    int32_t timeline = s.new_timeline(0);
    ASSERT_TRUE(writer >= 0);
    ASSERT_TRUE(timeline >= 0);

    EXPECT_EQ(look_up_and_release(writer, GPU_RIGHT_READ), fs::ERR_BADF);
    EXPECT_EQ(look_up_and_release(timeline, GPU_RIGHT_READ), fs::ERR_BADF);
    EXPECT_EQ(look_up_and_release(-1, GPU_RIGHT_READ), fs::ERR_BADF);
    EXPECT_EQ(look_up_and_release(writer, GPU_RIGHT_WRITE), fs::OK);

    close_handle(timeline);
    close_handle(writer);
    close_handle(handle);
    EXPECT_TRUE(drain_reaper(s, g_refused_lookup_fake, 1));
}
