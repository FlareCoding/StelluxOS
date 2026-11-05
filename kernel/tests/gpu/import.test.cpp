#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fake_backend.h"
#include "sync/mutex.h"
#include "syscall/handlers/sys_mmap.h"

TEST_SUITE(gpu_import);

using gpu_test::close_handle;
using gpu_test::FAKE_VA_START;
using gpu_test::import_request;
using gpu_test::session;
using gpu_test::shared_memory;
using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::start_pinned_task_on_another_cpu;
using test_helpers::unpin;

constexpr uint64_t PAGE = pmm::PAGE_SIZE;
constexpr uint32_t BOTH_RIGHTS = resource::RIGHT_READ | resource::RIGHT_WRITE;
constexpr uint32_t SMALL = 16;
constexpr uint64_t SHORT_TIMEOUT_NS = 50ull * 1000 * 1000;
constexpr uint64_t PROT_READ_ARG = 0x1;
constexpr uint64_t MAP_SHARED_ARG = 0x01;

// A core call made on a task of its own while the test holds a shared memory object's mutex, so a call that waits for
// the mutex fails the test's bounded wait instead of hanging the run. Static per test, since a waiting task reads it
struct held_call {
    gpu::core_queue*       queue;
    sched::task*           owner;
    int32_t                handle;
    sched::task*           task;
    sync::atomic<uint32_t> returned;
};

static void progress_on_its_task(void* arg) {
    held_call& run = *static_cast<held_call*>(arg);
    gpu::progress(run.queue, 1);
    run.returned.store_release(1);
    sched::exit(0);
}

static void close_on_its_task(void* arg) {
    held_call& run = *static_cast<held_call*>(arg);
    resource::close(run.owner, static_cast<resource::handle_t>(run.handle));
    run.returned.store_release(1);
    sched::exit(0);
}

// Starts `entry` with `run` on another CPU and returns whether the call returned before the spin deadline
static bool returns_on_another_cpu(void (*entry)(void*), held_call& run) {
    run.task = start_pinned_task_on_another_cpu(entry, &run, "gpu_import_call", sched::TASK_FLAG_ELEVATED);
    return run.task && spin_wait(run.returned);
}

// Waits for the call once the test unlocked the mutex, then drops the test's reference to its task
static void finish_call(held_call& run) {
    if (run.task) {
        spin_wait(run.returned);
        unpin(run.task);
    }
}

// A shared mmap of the first page of the buffer `handle` names, returning the address or a syscall code
static int64_t map_for_cpu(session& s, int32_t handle) {
    test_helpers::user_space_scope scope(s.page.ctx);
    return sys_mmap(0, PAGE, PROT_READ_ARG, MAP_SHARED_ARG, static_cast<uint64_t>(handle), 0);
}

// An import the test expects to fail, returning the call's code. Any handle the call installed is closed, so a wrong
// success fails only its own test, not each later suite that counts handles
static int64_t refused_import(session& s, gpu_buffer_args& args) {
    int64_t rc = s.call(GPU_BUFFER_IMPORT, args);
    if (args.buffer >= 0) {
        close_handle(args.buffer);
    }

    return rc;
}

static int32_t refused_import(session& s, int32_t source) {
    gpu_buffer_args args = import_request(source);
    return static_cast<int32_t>(refused_import(s, args));
}

// Closes `imported` once its device's failure is reported and returns whether the backend got its memory back and
// kept it with the pin
static bool release_keeps_the_pin(gpu_test::fake_backend& fake, int32_t imported, const shared_memory& shared) {
    bool reported = spin_wait_ge(fake.failures_reported, 1);
    close_handle(imported);
    bool released = spin_wait_ge(fake.released_count, 1);

    return reported && released && fake.freed_count.load_acquire() == 0 && shared.pins() == 1;
}

static gpu_test::fake_backend g_buffer_import_fake;

// Importing a GPU buffer writes back its size, placement, flags and device id and gives another handle to it with
// the source's rights. The handle keeps the buffer once the source closed, and the backend is asked nothing
TEST(gpu_import, a_gpu_buffer_import_gives_another_handle_to_it_with_the_sources_rights) {
    session s(g_buffer_import_fake);
    ASSERT_TRUE(s.ready());
    int32_t context = s.new_context();
    int32_t buffer = s.new_buffer(PAGE + 1, GPU_PLACEMENT_VIDEO, GPU_BUFFER_MAPPABLE);
    ASSERT_TRUE(context >= 0 && buffer >= 0);
    gpu::memory* memory = g_buffer_import_fake.last_created;

    gpu_buffer_args report = import_request(buffer);
    EXPECT_EQ(s.call(GPU_BUFFER_IMPORT, report), static_cast<int64_t>(0));
    EXPECT_EQ(report.size, 2 * PAGE);
    EXPECT_EQ(report.placement, static_cast<uint32_t>(GPU_PLACEMENT_VIDEO));
    EXPECT_EQ(report.flags, static_cast<uint32_t>(GPU_BUFFER_MAPPABLE));
    EXPECT_EQ(report.device_id, 1u);

    int32_t reader = s.restrict_to(buffer, GPU_RIGHT_READ);
    int32_t writer = s.restrict_to(buffer, GPU_RIGHT_WRITE);
    ASSERT_TRUE(reader >= 0 && writer >= 0);
    int32_t read_only = s.import(reader);
    EXPECT_EQ(refused_import(s, writer), static_cast<int32_t>(syscall::EBADF));

    close_handle(buffer);
    close_handle(reader);
    close_handle(writer);
    ASSERT_TRUE(read_only >= 0);
    EXPECT_EQ(s.map(context, read_only, FAKE_VA_START, 0, PAGE, 0), syscall::EBADF);
    EXPECT_EQ(s.map(context, read_only, FAKE_VA_START, 0, PAGE, GPU_MAP_READ_ONLY), static_cast<int64_t>(0));

    EXPECT_EQ(s.map(context, report.buffer, FAKE_VA_START + PAGE, PAGE, PAGE, 0), static_cast<int64_t>(0));
    EXPECT_TRUE(g_buffer_import_fake.last_map.mem == memory);
    EXPECT_EQ(g_buffer_import_fake.create_calls, 1u);
    EXPECT_EQ(g_buffer_import_fake.wrap_calls, 0u);

    close_handle(read_only);
    close_handle(report.buffer);
    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_buffer_import_fake.released_count, 1));
}

static gpu_test::fake_backend g_owner_fake;
static gpu_test::fake_backend g_other_fake;

// Importing a GPU buffer through another device's file fails with EINVAL, also once that device failed, and with
// EBADF through a handle without read, the imports asking neither backend anything
TEST(gpu_import, an_import_through_another_devices_file_is_refused) {
    session owner(g_owner_fake);
    session other(g_other_fake);
    ASSERT_TRUE(owner.ready() && other.ready());
    int32_t buffer = owner.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    int32_t writer = owner.restrict_to(buffer, GPU_RIGHT_WRITE);
    ASSERT_TRUE(buffer >= 0 && writer >= 0);

    gpu_buffer_args args = import_request(buffer);
    EXPECT_EQ(refused_import(other, args), syscall::EINVAL);
    EXPECT_EQ(args.buffer, -1);
    EXPECT_EQ(refused_import(other, writer), static_cast<int32_t>(syscall::EBADF));
    EXPECT_EQ(g_owner_fake.create_calls, 1u);
    EXPECT_EQ(g_owner_fake.wrap_calls + g_other_fake.create_calls + g_other_fake.wrap_calls, 0u);

    g_other_fake.fail_zeroing = true;
    EXPECT_EQ(other.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0), static_cast<int32_t>(syscall::EIO));
    ASSERT_TRUE(spin_wait_ge(g_other_fake.failures_reported, 1));
    EXPECT_EQ(refused_import(other, buffer), static_cast<int32_t>(syscall::EINVAL));
    EXPECT_EQ(g_owner_fake.wrap_calls + g_other_fake.wrap_calls, 0u);

    close_handle(writer);
    close_handle(buffer);
}

static gpu_test::fake_backend g_shared_fake;

// Importing shared memory hands the backend the object's pages with a pin and gives a SYSTEM buffer of their size with
// the source's rights. An mmap of its handle fails with EINVAL, and each release drops its pin
TEST(gpu_import, a_shared_memory_import_wraps_the_pages_with_the_sources_rights_and_its_release_drops_the_pin) {
    session s(g_shared_fake);
    shared_memory shared(PAGE + 1, BOTH_RIGHTS);
    shared_memory readable(PAGE, resource::RIGHT_READ);
    int32_t context = s.new_context();
    ASSERT_TRUE(s.ready() && shared.ready() && readable.ready() && context >= 0);
    uint32_t references = shared.references();

    gpu_buffer_args report = import_request(shared.handle);
    EXPECT_EQ(s.call(GPU_BUFFER_IMPORT, report), static_cast<int64_t>(0));
    EXPECT_EQ(report.size, 2 * PAGE);
    EXPECT_EQ(report.placement, static_cast<uint32_t>(GPU_PLACEMENT_SYSTEM));
    EXPECT_EQ(report.flags, 0u);
    EXPECT_EQ(report.device_id, 1u);

    EXPECT_EQ(shared.references(), references);
    EXPECT_EQ(g_shared_fake.last_wrap_count, static_cast<size_t>(2));
    EXPECT_EQ(g_shared_fake.last_wrap_pages[0], shared.page(0));
    EXPECT_EQ(g_shared_fake.last_wrap_pages[1], shared.page(1));
    EXPECT_EQ(shared.pins(), static_cast<size_t>(1));

    EXPECT_EQ(map_for_cpu(s, report.buffer), syscall::EINVAL);
    EXPECT_EQ(s.map(context, report.buffer, FAKE_VA_START, 0, 2 * PAGE, 0), static_cast<int64_t>(0));
    EXPECT_TRUE(g_shared_fake.last_map.mem == g_shared_fake.last_wrapped);

    uint64_t read_only_va = FAKE_VA_START + 2 * PAGE;
    int32_t read_only = s.import(readable.handle);
    ASSERT_TRUE(read_only >= 0);
    EXPECT_EQ(s.map(context, read_only, read_only_va, 0, PAGE, 0), syscall::EBADF);
    EXPECT_EQ(s.map(context, read_only, read_only_va, 0, PAGE, GPU_MAP_READ_ONLY), static_cast<int64_t>(0));
    EXPECT_EQ(g_shared_fake.wrap_calls, 2u);

    close_handle(report.buffer);
    close_handle(read_only);
    EXPECT_EQ(shared.pins(), static_cast<size_t>(1));

    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_shared_fake.released_count, 2));
    EXPECT_EQ(g_shared_fake.freed_count.load_acquire(), 2u);
    EXPECT_EQ(shared.pins() + readable.pins(), static_cast<size_t>(0));
}

static gpu_test::fake_backend g_truncate_fake;

// Shared memory truncated to nothing keeps the pages an import wrapped until the buffer's release frees them
TEST(gpu_import, a_truncated_shared_memory_keeps_the_wrapped_pages_until_the_buffers_release) {
    session s(g_truncate_fake);
    shared_memory shared(2 * PAGE, BOTH_RIGHTS);
    ASSERT_TRUE(s.ready() && shared.ready());
    int32_t imported = s.import(shared.handle);
    ASSERT_TRUE(imported >= 0);

    sync::mutex_lock(shared.object->lock);
    int32_t truncated = mm::shmem_resize_locked(shared.object, 0);
    sync::mutex_unlock(shared.object->lock);
    EXPECT_EQ(truncated, mm::SHMEM_OK);
    EXPECT_EQ(shared.page(0), g_truncate_fake.last_wrap_pages[0]);
    EXPECT_EQ(shared.page(1), g_truncate_fake.last_wrap_pages[1]);

    close_handle(imported);
    EXPECT_TRUE(spin_wait_ge(g_truncate_fake.released_count, 1));
    EXPECT_EQ(shared.page(0), static_cast<pmm::phys_addr_t>(0));
    EXPECT_EQ(shared.page(1), static_cast<pmm::phys_addr_t>(0));
}

static gpu_test::fake_backend g_import_arguments_fake;
static gpu_test::fake_backend g_video_only_fake;

// An import refuses a source that is not a GPU buffer or shared memory handle with read (EBADF), a nonzero context or
// backend_id, shared memory on a device without SYSTEM or an empty object (EINVAL), with no handle, pin or wrap
TEST(gpu_import, an_import_refuses_each_invalid_source_and_argument) {
    g_video_only_fake.placements = 1u << GPU_PLACEMENT_VIDEO;
    session s(g_import_arguments_fake);
    session video_only(g_video_only_fake);
    shared_memory shared(PAGE, BOTH_RIGHTS);
    shared_memory write_only(PAGE, resource::RIGHT_WRITE);
    shared_memory empty(0, BOTH_RIGHTS);

    int32_t timeline = s.new_timeline(0);
    ASSERT_TRUE(s.ready() && video_only.ready() && timeline >= 0);
    ASSERT_TRUE(shared.ready() && write_only.ready() && empty.ready());
    uint32_t references = shared.references() + empty.references();

    constexpr size_t CASES = 8;
    gpu_buffer_args refused[CASES] = {
        import_request(write_only.handle),
        import_request(timeline),
        import_request(-1),
        import_request(-1),
        import_request(-1),
        import_request(shared.handle),
        import_request(shared.handle),
        import_request(empty.handle),
    };

    // The context and backend_id are checked before the source
    refused[3].context = 1;
    refused[4].backend_id = 1;
    refused[5].context = 1;
    refused[6].backend_id = 1ull << 32;
    const int64_t expected[CASES] = {syscall::EBADF, syscall::EBADF, syscall::EBADF, syscall::EINVAL, syscall::EINVAL,
                                     syscall::EINVAL, syscall::EINVAL, syscall::EINVAL};

    for (size_t i = 0; i < CASES; i++) {
        EXPECT_EQ(refused_import(s, refused[i]), expected[i]);
        EXPECT_EQ(refused[i].buffer, -1);
    }

    // On a device without SYSTEM a GPU buffer still imports
    gpu_buffer_args video = import_request(video_only.new_buffer(PAGE, GPU_PLACEMENT_VIDEO, 0));
    EXPECT_EQ(video_only.call(GPU_BUFFER_IMPORT, video), static_cast<int64_t>(0));
    EXPECT_EQ(video.placement, static_cast<uint32_t>(GPU_PLACEMENT_VIDEO));
    close_handle(video.buffer);
    close_handle(video.source);
    EXPECT_TRUE(spin_wait_ge(g_video_only_fake.released_count, 1));

    // The read right and the kind are checked before SYSTEM
    gpu_buffer_args no_system = import_request(shared.handle);
    gpu_buffer_args unreadable = import_request(write_only.handle);
    EXPECT_EQ(refused_import(video_only, no_system), syscall::EINVAL);
    EXPECT_EQ(refused_import(video_only, unreadable), syscall::EBADF);
    EXPECT_EQ(refused_import(video_only, timeline), static_cast<int32_t>(syscall::EBADF));
    EXPECT_TRUE(no_system.buffer == -1 && unreadable.buffer == -1);

    // SYSTEM is checked before the device's failure
    g_video_only_fake.fail_zeroing = true;
    EXPECT_EQ(video_only.new_buffer(PAGE, GPU_PLACEMENT_VIDEO, 0), static_cast<int32_t>(syscall::EIO));
    ASSERT_TRUE(spin_wait_ge(g_video_only_fake.failures_reported, 1));
    EXPECT_EQ(refused_import(video_only, shared.handle), static_cast<int32_t>(syscall::EINVAL));

    EXPECT_EQ(g_import_arguments_fake.wrap_calls + g_video_only_fake.wrap_calls, 0u);
    EXPECT_EQ(shared.pins() + write_only.pins() + empty.pins(), static_cast<size_t>(0));
    EXPECT_EQ(shared.references() + empty.references(), references);

    close_handle(timeline);
}

static gpu_test::fake_backend g_import_memory_fake;

// A shared memory import without memory for its page list, or whose wrap the backend refuses for want of memory,
// returns ENOMEM with no handle and no pin left, and a later import of the object succeeds
TEST(gpu_import, a_shared_memory_import_without_memory_leaves_no_pin) {
    session s(g_import_memory_fake);
    shared_memory shared(2 * PAGE, BOTH_RIGHTS);
    ASSERT_TRUE(s.ready() && shared.ready());

    gpu::__dbg_test_fail_next_allocation();
    gpu_buffer_args listless = import_request(shared.handle);
    EXPECT_EQ(refused_import(s, listless), syscall::ENOMEM);
    EXPECT_EQ(listless.buffer, -1);
    EXPECT_EQ(g_import_memory_fake.wrap_calls, 0u);
    EXPECT_EQ(shared.pins(), static_cast<size_t>(0));

    g_import_memory_fake.wrap_result = gpu::ERR_NO_MEMORY;
    gpu_buffer_args refused = import_request(shared.handle);
    EXPECT_EQ(refused_import(s, refused), syscall::ENOMEM);
    EXPECT_EQ(refused.buffer, -1);
    EXPECT_EQ(g_import_memory_fake.wrap_calls, 1u);
    EXPECT_EQ(shared.pins(), static_cast<size_t>(0));

    g_import_memory_fake.wrap_result = gpu::OK;
    int32_t imported = s.import(shared.handle);
    EXPECT_TRUE(imported >= 0);
    EXPECT_EQ(shared.pins(), static_cast<size_t>(1));

    close_handle(imported);
    EXPECT_TRUE(spin_wait_ge(g_import_memory_fake.released_count, 1));
    EXPECT_EQ(shared.pins(), static_cast<size_t>(0));
}

static gpu_test::fake_backend g_zeroing_fake;

// A shared memory buffer's release after a failed zeroing reaches the backend, which keeps its memory with the pin
TEST(gpu_import, a_shared_memory_buffer_released_after_a_failed_zeroing_keeps_its_pin) {
    session s(g_zeroing_fake);
    shared_memory shared(PAGE, BOTH_RIGHTS);
    ASSERT_TRUE(s.ready() && shared.ready());
    int32_t imported = s.import(shared.handle);
    ASSERT_TRUE(imported >= 0);

    g_zeroing_fake.fail_zeroing = true;
    EXPECT_EQ(s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0), static_cast<int32_t>(syscall::EIO));
    EXPECT_TRUE(release_keeps_the_pin(g_zeroing_fake, imported, shared));
}

static gpu_test::fake_backend g_invalidate_fake;

// A shared memory buffer's release after a failed invalidate reaches the backend, which keeps its memory with the pin
TEST(gpu_import, a_shared_memory_buffer_released_after_a_failed_invalidate_keeps_its_pin) {
    session s(g_invalidate_fake);
    shared_memory shared(PAGE, BOTH_RIGHTS);
    int32_t context = s.new_context();
    ASSERT_TRUE(s.ready() && shared.ready() && context >= 0);
    int32_t imported = s.import(shared.handle);
    ASSERT_TRUE(imported >= 0);

    ASSERT_EQ(s.map(context, imported, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    g_invalidate_fake.fail_unmap = true;
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, PAGE), syscall::EIO);
    EXPECT_TRUE(release_keeps_the_pin(g_invalidate_fake, imported, shared));

    close_handle(context);
}

static gpu_test::fake_backend g_wrap_deadline_fake;

// An import whose wrap the fake fails at its deadline returns EIO with its pin kept, a release after the failure keeps
// its pin too, and a later import of either kind, an empty object's included, returns EIO before the backend
TEST(gpu_import, a_wrap_failed_at_its_deadline_keeps_its_pin_and_fails_later_imports) {
    session s(g_wrap_deadline_fake);
    shared_memory released(PAGE, BOTH_RIGHTS);
    shared_memory failed(PAGE, BOTH_RIGHTS);
    shared_memory empty(0, BOTH_RIGHTS);
    int32_t buffer = s.new_buffer(PAGE, GPU_PLACEMENT_SYSTEM, 0);
    ASSERT_TRUE(s.ready() && released.ready() && failed.ready() && empty.ready() && buffer >= 0);

    int32_t imported = s.import(released.handle);
    ASSERT_TRUE(imported >= 0);
    g_wrap_deadline_fake.fail_wrap = true;
    EXPECT_EQ(refused_import(s, failed.handle), static_cast<int32_t>(syscall::EIO));
    EXPECT_EQ(failed.pins(), static_cast<size_t>(1));
    EXPECT_TRUE(release_keeps_the_pin(g_wrap_deadline_fake, imported, released));

    EXPECT_EQ(refused_import(s, buffer), static_cast<int32_t>(syscall::EIO));
    EXPECT_EQ(refused_import(s, released.handle), static_cast<int32_t>(syscall::EIO));
    EXPECT_EQ(refused_import(s, empty.handle), static_cast<int32_t>(syscall::EIO));
    EXPECT_EQ(g_wrap_deadline_fake.wrap_calls, 2u);
    EXPECT_EQ(released.pins(), static_cast<size_t>(1));

    close_handle(buffer);
}

static gpu_test::fake_backend g_progress_fake;
static held_call g_progress_call = {};

// A shared memory buffer whose last reference an unmap marker held is released on the reaper once progress passes the
// marker, so progress returns while the test holds the object's mutex, and the release then drops the pin
TEST(gpu_import, a_shared_memory_buffer_dropped_in_progress_releases_its_pin_on_the_reaper) {
    session s(g_progress_fake);
    shared_memory shared(PAGE, BOTH_RIGHTS);
    int32_t context = s.new_context();
    ASSERT_TRUE(s.ready() && shared.ready() && context >= 0);
    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));

    int32_t imported = s.import(shared.handle);
    ASSERT_TRUE(imported >= 0);
    ASSERT_EQ(s.map(context, imported, FAKE_VA_START, 0, PAGE, 0), static_cast<int64_t>(0));
    close_handle(imported);
    EXPECT_EQ(s.submit(context, 0, 0x31, SMALL, nullptr, 0, nullptr, 0), static_cast<int64_t>(0));
    EXPECT_EQ(s.unmap(context, FAKE_VA_START, PAGE), static_cast<int64_t>(0));

    EXPECT_EQ(g_progress_fake.released_count.load_acquire(), 0u);
    g_progress_call.queue = g_progress_fake.queues[0].core;
    sync::mutex_lock(shared.object->lock);
    bool returned = returns_on_another_cpu(progress_on_its_task, g_progress_call);
    sync::mutex_unlock(shared.object->lock);
    finish_call(g_progress_call);

    EXPECT_TRUE(returned);
    EXPECT_TRUE(spin_wait_ge(g_progress_fake.released_count, 1));
    EXPECT_EQ(shared.pins(), static_cast<size_t>(0));

    close_handle(context);
    EXPECT_TRUE(spin_wait_ge(g_progress_fake.spaces_destroyed, 1));
}

static gpu_test::fake_backend g_blocked_reaper_fake;
static held_call g_blocked_reaper_close = {};

// While the reaper waits in a pin's release for a mutex the test holds, a closed context's work past its deadline has
// its queue asked to stop, and the queue's stop ends the work failed once the mutex is free
TEST(gpu_import, a_deadline_stops_a_closed_contexts_queue_while_the_reaper_waits_in_a_pins_release) {
    gpu_test::fake_backend& fake = g_blocked_reaper_fake;
    fake.submit_timeout_ns = SHORT_TIMEOUT_NS;
    session s(fake);
    shared_memory shared(PAGE, BOTH_RIGHTS);

    int32_t context = s.new_context();
    int32_t timeline = s.new_timeline(0);
    ASSERT_TRUE(s.ready() && shared.ready() && context >= 0 && timeline >= 0);
    ASSERT_EQ(s.new_queue(context, GPU_QUEUE_GRAPHICS), static_cast<int64_t>(0));

    int32_t imported = s.import(shared.handle);
    ASSERT_TRUE(imported >= 0);
    gpu_point done = gpu_test::point(timeline, 1);
    g_blocked_reaper_close.owner = sched::current();
    g_blocked_reaper_close.handle = imported;

    // The release queued first blocks the reaper, so the teardown's first stage cannot run before the unlock
    sync::mutex_lock(shared.object->lock);
    bool closed = returns_on_another_cpu(close_on_its_task, g_blocked_reaper_close);
    int64_t submitted = s.submit(context, 0, 0x41, SMALL, nullptr, 0, &done, 1);
    close_handle(context);

    bool stopped = spin_wait_ge(fake.stops_asked, 1);
    uint32_t reaped = fake.released_count.load_acquire() + fake.spaces_destroyed.load_acquire();
    sync::mutex_unlock(shared.object->lock);
    finish_call(g_blocked_reaper_close);

    EXPECT_TRUE(closed);
    EXPECT_EQ(submitted, static_cast<int64_t>(0));
    EXPECT_TRUE(stopped);
    EXPECT_EQ(reaped, 0u);

    EXPECT_TRUE(spin_wait_ge(fake.spaces_destroyed, 1));
    EXPECT_EQ(fake.stops_asked.load_acquire(), 1u);
    EXPECT_EQ(shared.pins(), static_cast<size_t>(0));

    gpu_timeline_args reached = gpu_test::timeline_args(timeline, 0);
    EXPECT_EQ(s.call(GPU_TIMELINE_QUERY, reached), static_cast<int64_t>(0));
    EXPECT_EQ(reached.value, static_cast<uint64_t>(1));
    EXPECT_EQ(reached.flags, static_cast<uint32_t>(GPU_TIMELINE_FAILED));

    close_handle(timeline);
}
