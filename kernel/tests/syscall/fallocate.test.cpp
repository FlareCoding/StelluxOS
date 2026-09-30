#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_memfd.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "resource/providers/shmem_provider.h"
#include "fs/file.h"
#include "fs/fstypes.h"
#include "fs/node.h"
#include "mm/shmem.h"
#include "sched/sched.h"
#include "sched/task.h"

TEST_SUITE(fallocate);

using test_helpers::user_page;
using test_helpers::user_space_scope;

static constexpr uint64_t FALLOC_FL_ALLOCATE_RANGE = 0;
static constexpr uint64_t FALLOC_FL_KEEP_SIZE = 1;
static constexpr int64_t RANGE_OFFSET = 4096;
static constexpr int64_t RANGE_LENGTH = 8192;

static int64_t call_fallocate(resource::handle_t h, uint64_t mode, int64_t offset, int64_t length) {
    return sys_fallocate(static_cast<uint64_t>(h), mode, static_cast<uint64_t>(offset),
                         static_cast<uint64_t>(length), 0, 0);
}

static int64_t create_pipe_into(user_page& page) {
    user_space_scope scope(page.ctx);
    return sys_pipe2(page.addr, 0, 0, 0, 0, 0);
}

static size_t read_object_size(resource::handle_t h) {
    resource::resource_object* obj = nullptr;
    if (resource::get_handle_object(sched::current()->handles, h, 0, &obj) != resource::HANDLE_OK) {
        return 0;
    }

    size_t size = 0;
    if (obj->type == resource::resource_type::SHMEM) {
        size = resource::shmem_provider::get_shmem_backing(obj)->m_size;
    } else {
        size = resource::file_provider::get_file(obj)->get_node()->size();
    }

    resource::resource_release(obj);

    return size;
}

static void expect_allocation_to_grow_but_never_shrink(resource::handle_t h) {
    EXPECT_EQ(call_fallocate(h, FALLOC_FL_ALLOCATE_RANGE, RANGE_OFFSET, RANGE_LENGTH), static_cast<int64_t>(0));
    EXPECT_EQ(read_object_size(h), static_cast<size_t>(RANGE_OFFSET + RANGE_LENGTH));

    EXPECT_EQ(call_fallocate(h, FALLOC_FL_ALLOCATE_RANGE, 0, RANGE_LENGTH), static_cast<int64_t>(0));
    EXPECT_EQ(read_object_size(h), static_cast<size_t>(RANGE_OFFSET + RANGE_LENGTH));
}

TEST(fallocate, files_and_memory_files_grow_to_cover_the_range) {
    sched::task* task = sched::current();

    resource::handle_t file = -1;
    ASSERT_EQ(resource::open(task, "/fallocate_file", fs::O_CREAT | fs::O_RDWR, &file), resource::OK);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(resource::shmem_provider::create_shmem_resource(0, &obj), resource::OK);
    resource::handle_t memory_file = -1;
    ASSERT_EQ(resource::alloc_task_handle(task, obj, resource::resource_type::SHMEM,
                                          resource::RIGHT_READ | resource::RIGHT_WRITE, &memory_file),
              resource::HANDLE_OK);
    resource::resource_release(obj);

    expect_allocation_to_grow_but_never_shrink(file);
    expect_allocation_to_grow_but_never_shrink(memory_file);

    resource::close(task, file);
    resource::close(task, memory_file);
}

TEST(fallocate, other_modes_bad_ranges_and_non_regular_files_are_refused) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t file = -1;
    ASSERT_EQ(resource::open(task, "/fallocate_refused", fs::O_CREAT | fs::O_RDWR, &file), resource::OK);

    resource::handle_t read_only_file = -1;
    ASSERT_EQ(resource::open(task, "/fallocate_refused", fs::O_RDONLY, &read_only_file), resource::OK);

    resource::handle_t device = -1;
    ASSERT_EQ(resource::open(task, "/dev/null", fs::O_RDWR, &device), resource::OK);

    ASSERT_EQ(create_pipe_into(page), static_cast<int64_t>(0));
    int32_t pipe_read_end = page.at<int32_t>(0)[0];
    int32_t pipe_write_end = page.at<int32_t>(0)[1];

    EXPECT_EQ(call_fallocate(file, FALLOC_FL_KEEP_SIZE, 0, RANGE_LENGTH), syscall::EOPNOTSUPP);
    EXPECT_EQ(call_fallocate(file, FALLOC_FL_ALLOCATE_RANGE, -1, RANGE_LENGTH), syscall::EINVAL);
    EXPECT_EQ(call_fallocate(file, FALLOC_FL_ALLOCATE_RANGE, 0, 0), syscall::EINVAL);
    EXPECT_EQ(call_fallocate(file, FALLOC_FL_ALLOCATE_RANGE, fs::MAX_FILE_OFFSET, 1), syscall::EFBIG);
    EXPECT_EQ(call_fallocate(read_only_file, FALLOC_FL_ALLOCATE_RANGE, 0, RANGE_LENGTH), syscall::EBADF);
    EXPECT_EQ(call_fallocate(-1, FALLOC_FL_ALLOCATE_RANGE, 0, RANGE_LENGTH), syscall::EBADF);
    EXPECT_EQ(call_fallocate(pipe_write_end, FALLOC_FL_ALLOCATE_RANGE, 0, RANGE_LENGTH), syscall::ESPIPE);
    EXPECT_EQ(call_fallocate(device, FALLOC_FL_ALLOCATE_RANGE, 0, RANGE_LENGTH), syscall::ENODEV);

    resource::close(task, file);
    resource::close(task, read_only_file);
    resource::close(task, device);
    resource::close(task, pipe_read_end);
    resource::close(task, pipe_write_end);
}
