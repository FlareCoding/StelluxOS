#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_dup.h"
#include "syscall/handlers/sys_pipe.h"
#include "syscall/handlers/sys_pty.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/atomic.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(flock);

using test_helpers::user_page;
using test_helpers::user_space_scope;

constexpr uint64_t LOCK_SH = 1;
constexpr uint64_t LOCK_EX = 2;
constexpr uint64_t LOCK_NB = 4;
constexpr uint64_t LOCK_UN = 8;
constexpr uint64_t UNKNOWN_LOCK_OPERATION = 16;

constexpr uint64_t F_SETLK = 6;
constexpr int16_t F_WRLCK = 1;

// Files the suite creates, removed after its last test even when an assertion ends a test early
constexpr const char* SEPARATE_PATH = "/flock_separate";
constexpr const char* ACCESS_PATH = "/flock_access";
constexpr const char* DUP_PATH = "/flock_dup";
constexpr const char* CLOSE_PATH = "/flock_close";
constexpr const char* WAIT_PATH = "/flock_wait";
constexpr const char* KILL_PATH = "/flock_wait_kill";
constexpr const char* RECORDS_PATH = "/flock_records";
constexpr const char* MALFORMED_PATH = "/flock_malformed";
constexpr const char* CREATED_PATHS[] = {
    SEPARATE_PATH, ACCESS_PATH, DUP_PATH, CLOSE_PATH, WAIT_PATH, KILL_PATH, RECORDS_PATH, MALFORMED_PATH,
};

// struct flock as programs lay it out on both 64-bit targets
struct flock_test_record_lock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
};

static int32_t remove_created_files() {
    for (const char* path : CREATED_PATHS) {
        (void)fs::unlink(path);
    }

    return 0;
}

AFTER_ALL(flock, remove_created_files);

static int64_t flock_handle(resource::handle_t h, uint64_t operation) {
    return sys_flock(static_cast<uint64_t>(h), operation, 0, 0, 0, 0);
}

// An exclusive record lock over every byte of the file, taken through `h` with F_SETLK
static int64_t take_exclusive_record_lock(user_page& page, resource::handle_t h) {
    *page.at<flock_test_record_lock>(0) = {F_WRLCK, static_cast<int16_t>(fs::SEEK_SET), 0, 0, 0};
    user_space_scope scope(page.ctx);
    return sys_fcntl(static_cast<uint64_t>(h), F_SETLK, page.addr, 0, 0, 0);
}

static int64_t create_pipe_into(user_page& page) {
    user_space_scope scope(page.ctx);
    return sys_pipe2(page.addr, 0, 0, 0, 0, 0);
}

static int64_t create_pty_into(user_page& page) {
    user_space_scope scope(page.ctx);
    return sys_pty_create(page.addr, 0, 0, 0, 0, 0);
}

TEST(flock, separate_opens_of_one_file_exclude_each_other) {
    sched::task* task = sched::current();
    resource::handle_t first = -1;
    resource::handle_t second = -1;
    ASSERT_EQ(resource::open(task, SEPARATE_PATH, fs::O_CREAT | fs::O_RDWR, &first), resource::OK);
    ASSERT_EQ(resource::open(task, SEPARATE_PATH, fs::O_RDWR, &second), resource::OK);

    ASSERT_EQ(flock_handle(first, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(second, LOCK_EX | LOCK_NB), syscall::EAGAIN);
    EXPECT_EQ(flock_handle(second, LOCK_SH | LOCK_NB), syscall::EAGAIN);

    // pyserial takes the lock again each time it reconfigures a port
    EXPECT_EQ(flock_handle(first, LOCK_EX | LOCK_NB), 0);

    ASSERT_EQ(flock_handle(first, LOCK_UN), 0);
    EXPECT_EQ(flock_handle(second, LOCK_EX | LOCK_NB), 0);

    resource::close(task, first);
    resource::close(task, second);
}

TEST(flock, any_access_mode_takes_either_lock) {
    sched::task* task = sched::current();
    resource::handle_t created = -1;
    ASSERT_EQ(resource::open(task, ACCESS_PATH, fs::O_CREAT | fs::O_RDWR, &created), resource::OK);
    resource::close(task, created);

    resource::handle_t reader = -1;
    resource::handle_t writer = -1;
    ASSERT_EQ(resource::open(task, ACCESS_PATH, fs::O_RDONLY, &reader), resource::OK);
    ASSERT_EQ(resource::open(task, ACCESS_PATH, fs::O_WRONLY, &writer), resource::OK);

    EXPECT_EQ(flock_handle(reader, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(writer, LOCK_SH | LOCK_NB), syscall::EAGAIN);

    EXPECT_EQ(flock_handle(reader, LOCK_SH | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(writer, LOCK_SH | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(writer, LOCK_EX | LOCK_NB), syscall::EAGAIN);

    resource::close(task, reader);
    resource::close(task, writer);
}

// Serial ports are device nodes, which a flasher locks exclusively and a monitor holds shared
TEST(flock, device_nodes_take_whole_file_locks) {
    sched::task* task = sched::current();
    resource::handle_t flasher = -1;
    resource::handle_t monitor = -1;
    ASSERT_EQ(resource::open(task, "/dev/null", fs::O_RDWR, &flasher), resource::OK);
    ASSERT_EQ(resource::open(task, "/dev/null", fs::O_RDWR, &monitor), resource::OK);

    ASSERT_EQ(flock_handle(flasher, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(monitor, LOCK_SH | LOCK_NB), syscall::EAGAIN);

    ASSERT_EQ(flock_handle(flasher, LOCK_UN), 0);
    ASSERT_EQ(flock_handle(monitor, LOCK_SH | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(flasher, LOCK_EX | LOCK_NB), syscall::EAGAIN);

    resource::close(task, flasher);
    resource::close(task, monitor);
}

TEST(flock, duplicated_handles_share_one_lock) {
    sched::task* task = sched::current();
    resource::handle_t locked = -1;
    resource::handle_t other = -1;
    ASSERT_EQ(resource::open(task, DUP_PATH, fs::O_CREAT | fs::O_RDWR, &locked), resource::OK);
    ASSERT_EQ(resource::open(task, DUP_PATH, fs::O_RDWR, &other), resource::OK);

    int64_t duplicate = sys_dup(static_cast<uint64_t>(locked), 0, 0, 0, 0, 0);
    ASSERT_TRUE(duplicate >= 0);
    resource::handle_t copy = static_cast<resource::handle_t>(duplicate);

    ASSERT_EQ(flock_handle(locked, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(copy, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(flock_handle(other, LOCK_SH | LOCK_NB), syscall::EAGAIN);

    ASSERT_EQ(flock_handle(copy, LOCK_UN), 0);
    EXPECT_EQ(flock_handle(other, LOCK_SH | LOCK_NB), 0);

    resource::close(task, locked);
    resource::close(task, copy);
    resource::close(task, other);
}

TEST(flock, the_lock_ends_when_the_last_handle_to_its_description_closes) {
    sched::task* task = sched::current();
    resource::handle_t locked = -1;
    resource::handle_t other = -1;
    ASSERT_EQ(resource::open(task, CLOSE_PATH, fs::O_CREAT | fs::O_RDWR, &locked), resource::OK);
    ASSERT_EQ(resource::open(task, CLOSE_PATH, fs::O_RDWR, &other), resource::OK);

    int64_t duplicate = sys_dup(static_cast<uint64_t>(locked), 0, 0, 0, 0, 0);
    ASSERT_TRUE(duplicate >= 0);
    resource::handle_t copy = static_cast<resource::handle_t>(duplicate);
    ASSERT_EQ(flock_handle(locked, LOCK_EX | LOCK_NB), 0);

    resource::close(task, locked);
    EXPECT_EQ(flock_handle(other, LOCK_EX | LOCK_NB), syscall::EAGAIN);

    resource::close(task, copy);
    EXPECT_EQ(flock_handle(other, LOCK_EX | LOCK_NB), 0);

    resource::close(task, other);
}

static sync::atomic<uint32_t> g_waiter_done;
static sync::atomic<int64_t> g_waiter_result;

// A kernel task has a handle table of its own, so the waiter opens the file again, as a second description
static void lock_exclusive_through_own_open(void* path) {
    sched::task* self = sched::current();
    resource::handle_t h = -1;
    int64_t result = syscall::EBADF;
    if (resource::open(self, static_cast<const char*>(path), fs::O_RDWR, &h) == resource::OK) {
        result = flock_handle(h, LOCK_EX);
        resource::close(self, h);
    }

    g_waiter_result.store_relaxed(result);
    g_waiter_done.store_release(1);
    sched::exit(0);
}

static sched::task* start_waiting_flock(const char* path) {
    g_waiter_done.store_relaxed(0);
    return test_helpers::start_pinned_task(lock_exclusive_through_own_open, const_cast<char*>(path), "flock_wait");
}

TEST(flock, a_blocking_lock_waits_until_the_holder_unlocks) {
    sched::task* task = sched::current();
    resource::handle_t holder = -1;
    ASSERT_EQ(resource::open(task, WAIT_PATH, fs::O_CREAT | fs::O_RDWR, &holder), resource::OK);
    ASSERT_EQ(flock_handle(holder, LOCK_EX | LOCK_NB), 0);

    sched::task* waiter = start_waiting_flock(WAIT_PATH);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));
    EXPECT_EQ(g_waiter_done.load_acquire(), 0u);

    ASSERT_EQ(flock_handle(holder, LOCK_UN), 0);
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), static_cast<int64_t>(0));

    test_helpers::unpin(waiter);
    resource::close(task, holder);
}

TEST(flock, a_kill_ends_a_blocking_lock_with_erestartsys) {
    sched::task* task = sched::current();
    resource::handle_t holder = -1;
    ASSERT_EQ(resource::open(task, KILL_PATH, fs::O_CREAT | fs::O_RDWR, &holder), resource::OK);
    ASSERT_EQ(flock_handle(holder, LOCK_EX | LOCK_NB), 0);

    sched::task* waiter = start_waiting_flock(KILL_PATH);
    ASSERT_NOT_NULL(waiter);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(waiter));

    RUN_ELEVATED(sched::force_wake_for_kill(waiter));
    ASSERT_TRUE(test_helpers::spin_wait(g_waiter_done));
    EXPECT_EQ(g_waiter_result.load_acquire(), syscall::ERESTARTSYS);

    test_helpers::unpin(waiter);
    resource::close(task, holder);
}

TEST(flock, whole_file_locks_and_record_locks_never_conflict) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t flocked = -1;
    resource::handle_t recorded = -1;
    ASSERT_EQ(resource::open(task, RECORDS_PATH, fs::O_CREAT | fs::O_RDWR, &flocked), resource::OK);
    ASSERT_EQ(resource::open(task, RECORDS_PATH, fs::O_RDWR, &recorded), resource::OK);

    ASSERT_EQ(take_exclusive_record_lock(page, recorded), 0);
    EXPECT_EQ(flock_handle(flocked, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(take_exclusive_record_lock(page, recorded), 0);
    EXPECT_EQ(flock_handle(recorded, LOCK_SH | LOCK_NB), syscall::EAGAIN);

    resource::close(task, flocked);
    resource::close(task, recorded);
}

TEST(flock, malformed_operations_and_non_files_are_refused) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, MALFORMED_PATH, fs::O_CREAT | fs::O_RDWR, &h), resource::OK);

    EXPECT_EQ(flock_handle(h, 0), syscall::EINVAL);
    EXPECT_EQ(flock_handle(h, LOCK_NB), syscall::EINVAL);
    EXPECT_EQ(flock_handle(h, LOCK_SH | LOCK_EX), syscall::EINVAL);
    EXPECT_EQ(flock_handle(h, LOCK_SH | LOCK_UN), syscall::EINVAL);
    EXPECT_EQ(flock_handle(h, LOCK_SH | UNKNOWN_LOCK_OPERATION), syscall::EINVAL);
    EXPECT_EQ(flock_handle(h, LOCK_UN | LOCK_NB), 0);

    EXPECT_EQ(flock_handle(-1, LOCK_SH), syscall::EBADF);
    EXPECT_EQ(flock_handle(-1, 0), syscall::EINVAL);

    ASSERT_EQ(create_pipe_into(page), static_cast<int64_t>(0));
    int32_t pipe_read_end = page.at<int32_t>(0)[0];
    int32_t pipe_write_end = page.at<int32_t>(0)[1];
    EXPECT_EQ(flock_handle(pipe_read_end, LOCK_SH | LOCK_NB), syscall::EINVAL);

    ASSERT_EQ(create_pty_into(page), static_cast<int64_t>(0));
    int32_t pty_master = page.at<int32_t>(0)[0];
    int32_t pty_slave = page.at<int32_t>(0)[1];
    EXPECT_EQ(flock_handle(pty_slave, LOCK_EX | LOCK_NB), syscall::EINVAL);

    resource::close(task, pipe_read_end);
    resource::close(task, pipe_write_end);
    resource::close(task, pty_master);
    resource::close(task, pty_slave);
    resource::close(task, h);
}
