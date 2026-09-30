#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/providers/shmem_provider.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"

TEST_SUITE(fsync);

using test_helpers::user_page;
using test_helpers::user_space_scope;

static int64_t create_pipe_into(user_page& page) {
    user_space_scope scope(page.ctx);
    return sys_pipe2(page.addr, 0, 0, 0, 0, 0);
}

static void expect_both_syncs_to_return(resource::handle_t h, int64_t expected) {
    EXPECT_EQ(sys_fsync(static_cast<uint64_t>(h), 0, 0, 0, 0, 0), expected);
    EXPECT_EQ(sys_fdatasync(static_cast<uint64_t>(h), 0, 0, 0, 0, 0), expected);
}

TEST(fsync, files_directories_and_memory_files_sync) {
    sched::task* task = sched::current();

    resource::handle_t read_only_file = -1;
    ASSERT_EQ(resource::open(task, "/fsync_file", fs::O_CREAT | fs::O_RDONLY, &read_only_file), resource::OK);

    resource::handle_t directory = -1;
    ASSERT_EQ(resource::open(task, "/", fs::O_RDONLY, &directory), resource::OK);

    resource::resource_object* obj = nullptr;
    ASSERT_EQ(resource::shmem_provider::create_shmem_resource(0, &obj), resource::OK);
    resource::handle_t memory_file = -1;
    ASSERT_EQ(resource::alloc_task_handle(task, obj, resource::resource_type::SHMEM,
                                          resource::RIGHT_READ | resource::RIGHT_WRITE, &memory_file),
              resource::HANDLE_OK);
    resource::resource_release(obj);

    expect_both_syncs_to_return(read_only_file, 0);
    expect_both_syncs_to_return(directory, 0);
    expect_both_syncs_to_return(memory_file, 0);

    resource::close(task, read_only_file);
    resource::close(task, directory);
    resource::close(task, memory_file);
}

TEST(fsync, pipes_devices_and_bad_handles_are_refused) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(create_pipe_into(page), static_cast<int64_t>(0));
    int32_t pipe_read_end = page.at<int32_t>(0)[0];
    int32_t pipe_write_end = page.at<int32_t>(0)[1];

    resource::handle_t device = -1;
    ASSERT_EQ(resource::open(task, "/dev/null", fs::O_RDWR, &device), resource::OK);

    expect_both_syncs_to_return(pipe_read_end, syscall::EINVAL);
    expect_both_syncs_to_return(device, syscall::EINVAL);
    expect_both_syncs_to_return(-1, syscall::EBADF);

    resource::close(task, pipe_read_end);
    resource::close(task, pipe_write_end);
    resource::close(task, device);
}
