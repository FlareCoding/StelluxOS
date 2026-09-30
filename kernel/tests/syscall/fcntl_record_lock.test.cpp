#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_dup.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/providers/file_provider.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
#include "fs/record_lock_table.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(fcntl_record_lock);

using test_helpers::user_page;
using test_helpers::user_space_scope;

constexpr uint64_t F_GETLK = 5;
constexpr uint64_t F_SETLK = 6;
constexpr uint64_t F_SETLKW = 7;

constexpr int16_t F_RDLCK = 0;
constexpr int16_t F_WRLCK = 1;
constexpr int16_t F_UNLCK = 2;

constexpr int16_t UNKNOWN_LOCK_TYPE = 3;
constexpr int16_t UNKNOWN_WHENCE = 3;
constexpr int32_t OTHER_PROCESS_PID = 4242;
constexpr uint64_t FAR_PAST_THE_END = 1ULL << 40;

// struct flock as programs lay it out on both 64-bit targets
struct user_flock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
};

// Stands in for another process's handle table, since lock owners are compared by address
static char g_other_owner;

static int64_t fcntl_with_flock(user_page& page, resource::handle_t h, uint64_t cmd, const user_flock& flock) {
    *page.at<user_flock>(0) = flock;
    user_space_scope scope(page.ctx);
    return sys_fcntl(static_cast<uint64_t>(h), cmd, page.addr, 0, 0, 0);
}

// The table stays valid while the handle to its file stays open
static fs::record_lock_table* record_locks_of(sched::task* task, resource::handle_t h) {
    fs::record_lock_table* locks = nullptr;
    RUN_ELEVATED({
        resource::resource_object* obj = nullptr;
        if (resource::get_handle_object(task->handles, h, 0, &obj) == resource::HANDLE_OK) {
            locks = &resource::file_provider::get_file(obj)->get_node()->record_locks();
            resource::resource_release(obj);
        }
    });

    return locks;
}

static int32_t lock_as_other_process(fs::record_lock_table* locks, uint64_t start, uint64_t end) {
    fs::record_lock request = {&g_other_owner, fs::record_lock_type::exclusive, start, end, OTHER_PROCESS_PID};
    int32_t result = 0;
    RUN_ELEVATED(result = locks->try_lock(request));
    return result;
}

static bool find_lock_blocking_other_process(
    fs::record_lock_table* locks,
    uint64_t start,
    uint64_t end,
    fs::record_lock* out_holder
) {
    fs::record_lock request = {&g_other_owner, fs::record_lock_type::exclusive, start, end, OTHER_PROCESS_PID};
    bool found = false;
    RUN_ELEVATED(found = locks->find_conflict(request, out_holder));
    return found;
}

static void unlock_other_process(fs::record_lock_table* locks) {
    RUN_ELEVATED(locks->unlock_all(&g_other_owner));
}

static int64_t create_pipe_into(user_page& page) {
    user_space_scope scope(page.ctx);
    return sys_pipe2(page.addr, 0, 0, 0, 0, 0);
}

TEST(fcntl_record_lock, getlk_reports_the_lock_of_another_process) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_report", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);
    fs::record_lock_table* locks = record_locks_of(task, h);
    ASSERT_NOT_NULL(locks);
    ASSERT_EQ(lock_as_other_process(locks, 10, 19), fs::OK);

    ASSERT_EQ(fcntl_with_flock(page, h, F_GETLK, {F_RDLCK, fs::SEEK_SET, 0, 100, 0}), 0);
    user_flock* reported = page.at<user_flock>(0);
    EXPECT_EQ(reported->l_type, F_WRLCK);
    EXPECT_EQ(reported->l_whence, static_cast<int16_t>(fs::SEEK_SET));
    EXPECT_EQ(reported->l_start, static_cast<int64_t>(10));
    EXPECT_EQ(reported->l_len, static_cast<int64_t>(10));
    EXPECT_EQ(reported->l_pid, OTHER_PROCESS_PID);

    ASSERT_EQ(fcntl_with_flock(page, h, F_GETLK, {F_WRLCK, fs::SEEK_SET, 20, 0, 0}), 0);
    EXPECT_EQ(page.at<user_flock>(0)->l_type, F_UNLCK);

    EXPECT_EQ(fcntl_with_flock(page, h, F_SETLK, {F_RDLCK, fs::SEEK_SET, 15, 1, 0}), syscall::EAGAIN);

    unlock_other_process(locks);
    resource::close(task, h);
}

TEST(fcntl_record_lock, a_zero_length_lock_covers_the_file_however_far_it_grows) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_whole", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);
    fs::record_lock_table* locks = record_locks_of(task, h);
    ASSERT_NOT_NULL(locks);

    ASSERT_EQ(fcntl_with_flock(page, h, F_SETLKW, {F_WRLCK, fs::SEEK_SET, 0, 0, 0}), 0);

    fs::record_lock holder = {};
    ASSERT_TRUE(find_lock_blocking_other_process(locks, FAR_PAST_THE_END, FAR_PAST_THE_END, &holder));
    EXPECT_TRUE(holder.owner == task->handles);
    EXPECT_EQ(holder.end, static_cast<uint64_t>(fs::MAX_FILE_OFFSET));

    ASSERT_EQ(fcntl_with_flock(page, h, F_SETLK, {F_UNLCK, fs::SEEK_SET, 0, 0, 0}), 0);
    EXPECT_FALSE(find_lock_blocking_other_process(locks, 0, 0, &holder));

    resource::close(task, h);
}

TEST(fcntl_record_lock, ranges_count_from_the_offset_or_the_end_and_may_run_backwards) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    uint32_t flags = fs::O_CREAT | fs::O_TRUNC | fs::O_RDWR;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_range", flags, &h), resource::OK);
    fs::record_lock_table* locks = record_locks_of(task, h);
    ASSERT_NOT_NULL(locks);

    // A 100 byte file whose offset sits at 50
    char bytes[50] = {};
    ASSERT_EQ(resource::write(task, h, bytes, sizeof(bytes)), static_cast<ssize_t>(50));
    ASSERT_EQ(resource::write_at(task, h, bytes, sizeof(bytes), 50), static_cast<ssize_t>(50));

    ASSERT_EQ(fcntl_with_flock(page, h, F_SETLK, {F_WRLCK, fs::SEEK_CUR, 10, 5, 0}), 0);
    ASSERT_EQ(fcntl_with_flock(page, h, F_SETLK, {F_WRLCK, fs::SEEK_END, -10, -10, 0}), 0);

    fs::record_lock holder = {};
    ASSERT_TRUE(find_lock_blocking_other_process(locks, 60, 60, &holder));
    EXPECT_EQ(holder.start, static_cast<uint64_t>(60));
    EXPECT_EQ(holder.end, static_cast<uint64_t>(64));

    ASSERT_TRUE(find_lock_blocking_other_process(locks, 85, 85, &holder));
    EXPECT_EQ(holder.start, static_cast<uint64_t>(80));
    EXPECT_EQ(holder.end, static_cast<uint64_t>(89));

    EXPECT_FALSE(find_lock_blocking_other_process(locks, 65, 79, &holder));
    EXPECT_FALSE(find_lock_blocking_other_process(locks, 90, 90, &holder));

    EXPECT_EQ(fcntl_with_flock(page, h, F_SETLK, {F_WRLCK, fs::SEEK_SET, 5, -10, 0}), syscall::EINVAL);
    EXPECT_EQ(fcntl_with_flock(page, h, F_SETLK, {F_WRLCK, fs::SEEK_END, fs::MAX_FILE_OFFSET, 1, 0}),
              syscall::EOVERFLOW);

    resource::close(task, h);
}

TEST(fcntl_record_lock, a_lock_needs_the_access_it_guards) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t created = -1;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_access", fs::O_CREAT | fs::O_RDWR, &created), resource::OK);
    resource::close(task, created);

    resource::handle_t reader = -1;
    resource::handle_t writer = -1;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_access", fs::O_RDONLY, &reader), resource::OK);
    ASSERT_EQ(resource::open(task, "/fcntl_lock_access", fs::O_WRONLY, &writer), resource::OK);

    EXPECT_EQ(fcntl_with_flock(page, reader, F_SETLK, {F_WRLCK, fs::SEEK_SET, 0, 0, 0}), syscall::EBADF);
    EXPECT_EQ(fcntl_with_flock(page, reader, F_SETLK, {F_RDLCK, fs::SEEK_SET, 0, 0, 0}), 0);
    EXPECT_EQ(fcntl_with_flock(page, writer, F_SETLK, {F_RDLCK, fs::SEEK_SET, 0, 0, 0}), syscall::EBADF);
    EXPECT_EQ(fcntl_with_flock(page, writer, F_SETLK, {F_WRLCK, fs::SEEK_SET, 0, 0, 0}), 0);

    resource::close(task, reader);
    resource::close(task, writer);
}

TEST(fcntl_record_lock, closing_any_handle_to_the_file_releases_the_locks) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t locked = -1;
    resource::handle_t second = -1;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_close", fs::O_CREAT | fs::O_RDWR, &locked), resource::OK);
    ASSERT_EQ(resource::open(task, "/fcntl_lock_close", fs::O_RDWR, &second), resource::OK);
    fs::record_lock_table* locks = record_locks_of(task, locked);
    ASSERT_NOT_NULL(locks);

    ASSERT_EQ(fcntl_with_flock(page, locked, F_SETLK, {F_WRLCK, fs::SEEK_SET, 0, 0, 0}), 0);
    EXPECT_EQ(lock_as_other_process(locks, 0, 0), fs::ERR_AGAIN);

    resource::close(task, second);
    EXPECT_EQ(lock_as_other_process(locks, 0, 0), fs::OK);

    unlock_other_process(locks);
    resource::close(task, locked);
}

TEST(fcntl_record_lock, replacing_a_handle_with_dup2_releases_the_locks) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t locked = -1;
    resource::handle_t keeper = -1;
    resource::handle_t replacement = -1;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_dup2", fs::O_CREAT | fs::O_RDWR, &locked), resource::OK);
    ASSERT_EQ(resource::open(task, "/fcntl_lock_dup2", fs::O_RDWR, &keeper), resource::OK);
    ASSERT_EQ(resource::open(task, "/fcntl_lock_dup2_other", fs::O_CREAT | fs::O_RDWR, &replacement),
              resource::OK);
    fs::record_lock_table* locks = record_locks_of(task, keeper);
    ASSERT_NOT_NULL(locks);

    ASSERT_EQ(fcntl_with_flock(page, locked, F_SETLK, {F_WRLCK, fs::SEEK_SET, 0, 0, 0}), 0);
    EXPECT_EQ(lock_as_other_process(locks, 0, 0), fs::ERR_AGAIN);

    ASSERT_EQ(sys_dup2(static_cast<uint64_t>(replacement), static_cast<uint64_t>(locked), 0, 0, 0, 0),
              static_cast<int64_t>(locked));
    EXPECT_EQ(lock_as_other_process(locks, 0, 0), fs::OK);

    unlock_other_process(locks);
    resource::close(task, locked);
    resource::close(task, keeper);
    resource::close(task, replacement);
}

TEST(fcntl_record_lock, malformed_requests_and_non_files_are_refused) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/fcntl_lock_malformed", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);

    EXPECT_EQ(fcntl_with_flock(page, h, F_GETLK, {F_UNLCK, fs::SEEK_SET, 0, 0, 0}), syscall::EINVAL);
    EXPECT_EQ(fcntl_with_flock(page, h, F_SETLK, {UNKNOWN_LOCK_TYPE, fs::SEEK_SET, 0, 0, 0}), syscall::EINVAL);
    EXPECT_EQ(fcntl_with_flock(page, h, F_SETLK, {F_WRLCK, UNKNOWN_WHENCE, 0, 0, 0}), syscall::EINVAL);
    EXPECT_EQ(fcntl_with_flock(page, -1, F_SETLK, {F_WRLCK, fs::SEEK_SET, 0, 0, 0}), syscall::EBADF);

    ASSERT_EQ(create_pipe_into(page), static_cast<int64_t>(0));
    int32_t pipe_read_end = page.at<int32_t>(0)[0];
    int32_t pipe_write_end = page.at<int32_t>(0)[1];
    EXPECT_EQ(fcntl_with_flock(page, pipe_read_end, F_SETLK, {F_RDLCK, fs::SEEK_SET, 0, 0, 0}), syscall::EINVAL);

    resource::close(task, pipe_read_end);
    resource::close(task, pipe_write_end);
    resource::close(task, h);
}
