#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fake_backend.h"
#include "clock/clock.h"
#include "common/string.h"
#include "syscall/handlers/sys_mmap.h"

TEST_SUITE(gpu_device);

using gpu_test::close_handle;
using gpu_test::session;
using test_helpers::spin_wait_ge;
using test_helpers::user_space_scope;

constexpr uint32_t UNKNOWN_CALL = 0x48FFu;
constexpr uint64_t UNMAPPED_ADDRESS = 0;
constexpr uint32_t UNKNOWN_RIGHT = 0x4u;
constexpr uint64_t PROT_READ_ARG = 0x1;
constexpr uint64_t PROT_READ_WRITE_ARG = 0x3;

// The N of a node named gpuN
static uint64_t node_number(const char* name) {
    uint64_t number = 0;
    for (const char* digit = name + 3; *digit >= '0' && *digit <= '9'; digit++) {
        number = number * 10 + static_cast<uint64_t>(*digit - '0');
    }

    return number;
}

static gpu_test::fake_backend g_first_numbered_fake;
static gpu_test::fake_backend g_second_numbered_fake;

// Each backend registered takes the next /dev/gpuN, the core reads its facts once, a name that fills `name` comes back
// with a NUL as its last byte, and GPU_GET_MEMORY returns the backend's report
TEST(gpu_device, each_backend_takes_the_next_node_and_reports_its_facts_and_memory) {
    g_first_numbered_fake.long_name = true;
    g_second_numbered_fake.memory_report = {.video_free = 3 * pmm::PAGE_SIZE, .mappable_video_free = pmm::PAGE_SIZE};
    session first(g_first_numbered_fake);
    session second(g_second_numbered_fake);
    ASSERT_TRUE(first.ready());
    ASSERT_TRUE(second.ready());

    EXPECT_EQ(string::strncmp(first.dev->name(), "gpu", 3), 0);
    EXPECT_EQ(node_number(second.dev->name()), node_number(first.dev->name()) + 1);

    gpu_info info = {};
    EXPECT_EQ(second.call(GPU_GET_INFO, info), static_cast<int64_t>(0));
    EXPECT_EQ(second.call(GPU_GET_INFO, info), static_cast<int64_t>(0));
    EXPECT_EQ(g_second_numbered_fake.facts_reads, 1u);

    EXPECT_EQ(info.backend, GPU_BACKEND_FAKE);
    EXPECT_EQ(info.vendor_id, gpu_test::FAKE_VENDOR_ID);
    EXPECT_EQ(info.video_size, gpu_test::FAKE_VIDEO_SIZE);
    EXPECT_EQ(info.placements, gpu_test::VIDEO_AND_SYSTEM);
    EXPECT_STREQ(info.name, "fake GPU");

    EXPECT_EQ(first.call(GPU_GET_INFO, info), static_cast<int64_t>(0));
    EXPECT_EQ(string::strlen(info.name), sizeof(info.name) - 1);

    gpu_memory report = {};
    EXPECT_EQ(second.call(GPU_GET_MEMORY, report), static_cast<int64_t>(0));
    EXPECT_EQ(report.video_free, static_cast<uint64_t>(3 * pmm::PAGE_SIZE));
    EXPECT_EQ(report.mappable_video_free, static_cast<uint64_t>(pmm::PAGE_SIZE));
}

static gpu_test::fake_backend g_clock_fake;
static gpu_test::fake_backend g_clockless_fake;

// The GPU's reading comes with a CPU clock read during the call, a clock read from a GPU without a clock and an
// unknown call are ENOTTY, a clock read from a failed GPU is EIO, and an argument the kernel cannot write is EINVAL
TEST(gpu_device, read_timestamp_pairs_the_clocks_and_refuses_what_the_device_cannot_do) {
    g_clock_fake.clock_ns = 123456789;
    g_clockless_fake.clock_result = gpu::ERR_UNSUPPORTED;
    session s(g_clock_fake);
    session clockless(g_clockless_fake);
    ASSERT_TRUE(s.ready());
    ASSERT_TRUE(clockless.ready());

    gpu_timestamp stamp = {};
    uint64_t before = clock::now_ns();
    EXPECT_EQ(s.call(GPU_READ_TIMESTAMP, stamp), static_cast<int64_t>(0));
    uint64_t after = clock::now_ns();

    EXPECT_EQ(stamp.gpu_ns, static_cast<uint64_t>(123456789));
    EXPECT_GE(stamp.cpu_ns, before);
    EXPECT_LE(stamp.cpu_ns, after);

    EXPECT_EQ(clockless.call(GPU_READ_TIMESTAMP, stamp), syscall::ENOTTY);
    EXPECT_EQ(s.call_in_place(UNKNOWN_CALL), syscall::ENOTTY);
    g_clockless_fake.clock_result = gpu::ERR_DEVICE;
    EXPECT_EQ(clockless.call(GPU_READ_TIMESTAMP, stamp), syscall::EIO);

    user_space_scope scope(s.page.ctx);
    uint64_t file = static_cast<uint64_t>(s.file);
    EXPECT_EQ(sys_ioctl(file, GPU_READ_TIMESTAMP, UNMAPPED_ADDRESS, 0, 0, 0), syscall::EINVAL);
}

static gpu_test::fake_backend g_restricting_fake;

// A restricted handle holds only the rights asked for and cannot widen again, and no rights, an unknown right, nonzero
// padding and a handle of another kind or none are each refused
TEST(gpu_device, restrict_narrows_a_handle_and_refuses_to_widen_or_to_take_an_invalid_argument) {
    session s(g_restricting_fake);
    ASSERT_TRUE(s.ready());

    int32_t buffer = s.new_buffer(pmm::PAGE_SIZE, GPU_PLACEMENT_VIDEO, 0);
    ASSERT_TRUE(buffer >= 0);
    int32_t reader = s.restrict_to(buffer, GPU_RIGHT_READ);
    ASSERT_TRUE(reader >= 0);
    int32_t again = s.restrict_to(reader, GPU_RIGHT_READ);
    EXPECT_TRUE(again >= 0);

    EXPECT_EQ(s.restrict_to(reader, GPU_RIGHT_READ | GPU_RIGHT_WRITE), syscall::EINVAL);
    EXPECT_EQ(s.restrict_to(reader, GPU_RIGHT_WRITE), syscall::EINVAL);
    EXPECT_EQ(s.restrict_to(buffer, 0), syscall::EINVAL);
    EXPECT_EQ(s.restrict_to(buffer, UNKNOWN_RIGHT), syscall::EINVAL);
    EXPECT_EQ(s.restrict_to(s.file, GPU_RIGHT_READ), syscall::EBADF);

    gpu_restrict_args padded = gpu_test::restrict_args(buffer, GPU_RIGHT_READ);
    padded.padding = 1;
    EXPECT_EQ(s.call(GPU_HANDLE_RESTRICT, padded), syscall::EINVAL);
    EXPECT_EQ(padded.restricted, -1);

    close_handle(again);
    close_handle(reader);
    close_handle(buffer);
    EXPECT_EQ(s.restrict_to(buffer, GPU_RIGHT_READ), syscall::EBADF);
}

static gpu_test::fake_backend g_owning_fake;
static gpu_test::fake_backend g_other_fake;

// A buffer is restricted only through its own device's file, and a timeline through any GPU's file
TEST(gpu_device, restrict_takes_a_buffer_only_through_its_own_device_and_a_timeline_through_any) {
    session owner(g_owning_fake);
    session other(g_other_fake);
    ASSERT_TRUE(owner.ready());
    ASSERT_TRUE(other.ready());

    int32_t buffer = owner.new_buffer(pmm::PAGE_SIZE, GPU_PLACEMENT_VIDEO, 0);
    ASSERT_TRUE(buffer >= 0);
    int32_t timeline = owner.new_timeline(0);
    ASSERT_TRUE(timeline >= 0);

    EXPECT_EQ(other.restrict_to(buffer, GPU_RIGHT_READ), syscall::EINVAL);

    int32_t own_reader = owner.restrict_to(buffer, GPU_RIGHT_READ);
    EXPECT_TRUE(own_reader >= 0);
    int32_t timeline_reader = other.restrict_to(timeline, GPU_RIGHT_READ);
    EXPECT_TRUE(timeline_reader >= 0);

    close_handle(timeline_reader);
    close_handle(own_reader);
    close_handle(timeline);
    close_handle(buffer);
}

static gpu_test::fake_backend g_full_table_fake;
static resource::handle_t g_fillers[resource::DEFAULT_HANDLE_LIMIT];

// With the handle table full, each call that makes a handle fails with ENOMEM, the create writing no handle, and the
// backend gets back the memory of the create and the shared memory import that failed, the import's pin released
TEST(gpu_device, a_full_handle_table_fails_each_new_handle_with_enomem) {
    session s(g_full_table_fake);
    gpu_test::shared_memory shared(pmm::PAGE_SIZE, resource::RIGHT_READ | resource::RIGHT_WRITE);
    ASSERT_TRUE(s.ready() && shared.ready());
    int32_t buffer = s.new_buffer(pmm::PAGE_SIZE, GPU_PLACEMENT_VIDEO, 0);
    ASSERT_TRUE(buffer >= 0);

    sched::task* task = sched::current();
    resource::resource_object* file = nullptr;
    ASSERT_EQ(resource::get_handle_object(task->handles, s.file, 0, &file), resource::HANDLE_OK);
    uint32_t filled = 0;
    for (; filled < resource::DEFAULT_HANDLE_LIMIT; filled++) {
        if (resource::alloc_task_handle(task, file, file->type, 0, &g_fillers[filled]) != resource::HANDLE_OK) {
            break;
        }
    }

    gpu_buffer_args created = gpu_test::buffer_request(pmm::PAGE_SIZE, GPU_PLACEMENT_VIDEO, 0);
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, created), syscall::ENOMEM);
    EXPECT_EQ(created.buffer, -1);
    EXPECT_TRUE(spin_wait_ge(g_full_table_fake.released_count, 1));
    EXPECT_EQ(g_full_table_fake.last_released, static_cast<gpu::memory*>(g_full_table_fake.last_created));

    EXPECT_EQ(s.restrict_to(buffer, GPU_RIGHT_READ), static_cast<int32_t>(syscall::ENOMEM));
    EXPECT_EQ(s.new_timeline(0), static_cast<int32_t>(syscall::ENOMEM));
    EXPECT_EQ(s.import(buffer), static_cast<int32_t>(syscall::ENOMEM));

    EXPECT_EQ(s.import(shared.handle), static_cast<int32_t>(syscall::ENOMEM));
    EXPECT_TRUE(spin_wait_ge(g_full_table_fake.released_count, 2));
    EXPECT_EQ(g_full_table_fake.last_released, static_cast<gpu::memory*>(g_full_table_fake.last_wrapped));
    EXPECT_EQ(shared.pins(), static_cast<size_t>(0));

    for (uint32_t i = 0; i < filled; i++) {
        close_handle(g_fillers[i]);
    }

    resource::resource_release(file);
    close_handle(buffer);
}

static gpu_test::fake_backend g_read_only_fake;

// With the argument's page read-only, each call that makes a handle fails with EINVAL and leaves no handle open, and
// the backend gets back the memory of the create and the shared memory import that failed, the import's pin released
TEST(gpu_device, an_argument_the_kernel_cannot_write_back_leaves_no_handle) {
    session s(g_read_only_fake);
    gpu_test::shared_memory shared(pmm::PAGE_SIZE, resource::RIGHT_READ | resource::RIGHT_WRITE);
    ASSERT_TRUE(s.ready() && shared.ready());
    int32_t buffer = s.new_buffer(pmm::PAGE_SIZE, GPU_PLACEMENT_VIDEO, 0);
    ASSERT_TRUE(buffer >= 0);

    int32_t next = s.restrict_to(buffer, GPU_RIGHT_READ);
    ASSERT_TRUE(next >= 0);
    close_handle(next);

    ASSERT_EQ(s.protect_page(PROT_READ_ARG), static_cast<int64_t>(0));
    gpu_buffer_args created = gpu_test::buffer_request(pmm::PAGE_SIZE, GPU_PLACEMENT_VIDEO, 0);
    EXPECT_EQ(s.call(GPU_BUFFER_CREATE, created), syscall::EINVAL);
    EXPECT_EQ(s.restrict_to(buffer, GPU_RIGHT_READ), static_cast<int32_t>(syscall::EINVAL));
    EXPECT_EQ(s.new_timeline(0), static_cast<int32_t>(syscall::EINVAL));

    EXPECT_EQ(s.import(buffer), static_cast<int32_t>(syscall::EINVAL));
    EXPECT_EQ(s.import(shared.handle), static_cast<int32_t>(syscall::EINVAL));
    ASSERT_EQ(s.protect_page(PROT_READ_WRITE_ARG), static_cast<int64_t>(0));

    EXPECT_TRUE(spin_wait_ge(g_read_only_fake.released_count, 2));
    EXPECT_EQ(shared.pins(), static_cast<size_t>(0));
    int32_t after = s.restrict_to(buffer, GPU_RIGHT_READ);
    EXPECT_EQ(after, next);

    close_handle(after);
    close_handle(buffer);
}
