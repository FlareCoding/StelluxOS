#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "resource/resource.h"
#include "syscall/handlers/sys_dup.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/heap.h"
#include "common/string.h"
#include "fs/fstypes.h"

TEST_SUITE(resource_test);

TEST(resource_test, open_close_file_resource) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h = -1;
    int32_t rc = resource::open(task, "/resource_open_close", fs::O_CREAT | fs::O_RDWR, &h);
    ASSERT_EQ(rc, resource::OK);
    EXPECT_TRUE(h >= 0);

    EXPECT_EQ(resource::close(task, h), resource::OK);
    EXPECT_EQ(resource::close(task, h), resource::ERR_BADF);
}

TEST(resource_test, read_write_roundtrip) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    const char* msg = "resource-hello";
    char buf[32] = {};

    resource::handle_t w = -1;
    ASSERT_EQ(resource::open(task, "/resource_rw", fs::O_CREAT | fs::O_RDWR, &w), resource::OK);
    ASSERT_EQ(resource::write(task, w, msg, 14), static_cast<ssize_t>(14));
    ASSERT_EQ(resource::close(task, w), resource::OK);

    resource::handle_t r = -1;
    ASSERT_EQ(resource::open(task, "/resource_rw", fs::O_RDONLY, &r), resource::OK);
    ASSERT_EQ(resource::read(task, r, buf, 14), static_cast<ssize_t>(14));
    EXPECT_STREQ(buf, "resource-hello");
    ASSERT_EQ(resource::close(task, r), resource::OK);
}

TEST(resource_test, independent_offsets_for_separate_opens) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    const char* msg = "abc";
    resource::handle_t setup = -1;
    ASSERT_EQ(resource::open(task, "/resource_offsets", fs::O_CREAT | fs::O_RDWR, &setup), resource::OK);
    ASSERT_EQ(resource::write(task, setup, msg, 3), static_cast<ssize_t>(3));
    ASSERT_EQ(resource::close(task, setup), resource::OK);

    resource::handle_t h1 = -1;
    resource::handle_t h2 = -1;
    ASSERT_EQ(resource::open(task, "/resource_offsets", fs::O_RDONLY, &h1), resource::OK);
    ASSERT_EQ(resource::open(task, "/resource_offsets", fs::O_RDONLY, &h2), resource::OK);

    char c1[2] = {};
    char c2[2] = {};
    ASSERT_EQ(resource::read(task, h1, c1, 1), static_cast<ssize_t>(1));
    ASSERT_EQ(resource::read(task, h2, c2, 1), static_cast<ssize_t>(1));
    EXPECT_STREQ(c1, "a");
    EXPECT_STREQ(c2, "a");

    ASSERT_EQ(resource::close(task, h1), resource::OK);
    ASSERT_EQ(resource::close(task, h2), resource::OK);
}

TEST(resource_test, rights_enforced_for_read_and_write) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t w = -1;
    ASSERT_EQ(resource::open(task, "/resource_rights", fs::O_CREAT | fs::O_WRONLY, &w), resource::OK);

    char buf[4] = {};
    EXPECT_EQ(resource::read(task, w, buf, 1), static_cast<ssize_t>(resource::ERR_ACCESS));
    EXPECT_EQ(resource::close(task, w), resource::OK);

    resource::handle_t r = -1;
    ASSERT_EQ(resource::open(task, "/resource_rights", fs::O_RDONLY, &r), resource::OK);
    EXPECT_EQ(resource::write(task, r, "x", 1), static_cast<ssize_t>(resource::ERR_ACCESS));
    EXPECT_EQ(resource::close(task, r), resource::OK);
}

TEST(resource_test, close_all_invalidates_existing_handles) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h1 = -1;
    resource::handle_t h2 = -1;
    ASSERT_EQ(resource::open(task, "/resource_close_all_1", fs::O_CREAT | fs::O_RDWR, &h1), resource::OK);
    ASSERT_EQ(resource::open(task, "/resource_close_all_2", fs::O_CREAT | fs::O_RDWR, &h2), resource::OK);

    resource::close_all(task);

    EXPECT_EQ(resource::close(task, h1), resource::ERR_BADF);
    EXPECT_EQ(resource::close(task, h2), resource::ERR_BADF);
}

TEST(resource_test, used_handle_slots_never_have_unknown_type) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, "/resource_slot_invariant", fs::O_CREAT | fs::O_RDWR, &h), resource::OK);

    bool saw_used = false;
    for (uint32_t i = 0; i < resource::MAX_TASK_HANDLES; i++) {
        const resource::handle_entry& entry = task->handles.entries[i];
        if (!entry.used) {
            continue;
        }
        saw_used = true;
        EXPECT_NOT_NULL(entry.obj);
        EXPECT_NE(entry.type, resource::resource_type::UNKNOWN);
        if (entry.obj) {
            EXPECT_EQ(entry.obj->type, entry.type);
        }
    }
    EXPECT_TRUE(saw_used);

    EXPECT_EQ(resource::close(task, h), resource::OK);
}

TEST(resource_test, missing_provider_ops_return_err_unsup) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    static const resource::resource_ops no_rw_ops = {
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr,
    };

    auto* read_obj = heap::kalloc_new<resource::resource_object>();
    ASSERT_NOT_NULL(read_obj);
    read_obj->type = resource::resource_type::FILE;
    read_obj->ops = &no_rw_ops;
    read_obj->impl = nullptr;

    resource::handle_t rh = -1;
    ASSERT_EQ(
        resource::alloc_handle(&task->handles, read_obj, resource::resource_type::FILE, resource::RIGHT_READ, &rh),
        resource::HANDLE_OK
    );
    resource::resource_release(read_obj);

    char byte = 0;
    EXPECT_EQ(resource::read(task, rh, &byte, 1), static_cast<ssize_t>(resource::ERR_UNSUP));
    EXPECT_EQ(resource::close(task, rh), resource::OK);

    auto* write_obj = heap::kalloc_new<resource::resource_object>();
    ASSERT_NOT_NULL(write_obj);
    write_obj->type = resource::resource_type::FILE;
    write_obj->ops = &no_rw_ops;
    write_obj->impl = nullptr;

    resource::handle_t wh = -1;
    ASSERT_EQ(
        resource::alloc_handle(&task->handles, write_obj, resource::resource_type::FILE, resource::RIGHT_WRITE, &wh),
        resource::HANDLE_OK
    );
    resource::resource_release(write_obj);

    EXPECT_EQ(resource::write(task, wh, "x", 1), static_cast<ssize_t>(resource::ERR_UNSUP));
    EXPECT_EQ(resource::close(task, wh), resource::OK);
}

namespace {

struct close_counter {
    uint32_t closes;
};

static void close_counter_close(resource::resource_object* obj) {
    auto* counter = static_cast<close_counter*>(obj->impl);
    if (counter) {
        counter->closes++;
    }
}

} // anonymous namespace

TEST(resource_test, terminal_release_invokes_close_once) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    static const resource::resource_ops close_counter_ops = {
        nullptr, nullptr, close_counter_close, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr,
    };

    close_counter counter{0};

    auto* obj = heap::kalloc_new<resource::resource_object>();
    ASSERT_NOT_NULL(obj);
    obj->type = resource::resource_type::FILE;
    obj->ops = &close_counter_ops;
    obj->impl = &counter;

    resource::handle_t h = -1;
    ASSERT_EQ(
        resource::alloc_handle(&task->handles, obj, resource::resource_type::FILE, resource::RIGHT_READ, &h),
        resource::HANDLE_OK
    );

    // Drop creator ownership; table entry keeps one reference.
    resource::resource_release(obj);
    EXPECT_EQ(counter.closes, 0u);

    EXPECT_EQ(resource::close(task, h), resource::OK);
    EXPECT_EQ(counter.closes, 1u);

    // Closing again must not re-run provider close.
    EXPECT_EQ(resource::close(task, h), resource::ERR_BADF);
    EXPECT_EQ(counter.closes, 1u);
}

TEST(resource_test, open_relative_path_returns_inval) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t h = -1;
    EXPECT_EQ(resource::open(task, "relative/path", fs::O_RDONLY, &h), resource::ERR_INVAL);
}

TEST(resource_test, open_returns_tablefull_when_handle_space_exhausted) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t handles[resource::MAX_TASK_HANDLES];
    for (uint32_t i = 0; i < resource::MAX_TASK_HANDLES; i++) {
        handles[i] = -1;
        ASSERT_EQ(resource::open(task, "/resource_full", fs::O_CREAT | fs::O_RDWR, &handles[i]), resource::OK);
    }

    resource::handle_t extra = -1;
    EXPECT_EQ(resource::open(task, "/resource_full", fs::O_CREAT | fs::O_RDWR, &extra), resource::ERR_TABLEFULL);

    for (uint32_t i = 0; i < resource::MAX_TASK_HANDLES; i++) {
        EXPECT_EQ(resource::close(task, handles[i]), resource::OK);
    }
}

// Direct handler invocation, since tests run as a kernel task with a
// live handle table
static int64_t call_dup(resource::handle_t h) {
    return sys_dup(static_cast<uint64_t>(h), 0, 0, 0, 0, 0);
}

static int64_t call_dup2(resource::handle_t old_h, resource::handle_t new_h) {
    return sys_dup2(static_cast<uint64_t>(old_h),
                    static_cast<uint64_t>(new_h), 0, 0, 0, 0);
}

static int64_t call_dup3(resource::handle_t old_h, resource::handle_t new_h,
                         uint64_t flags) {
    return sys_dup3(static_cast<uint64_t>(old_h),
                    static_cast<uint64_t>(new_h), flags, 0, 0, 0);
}

TEST(resource_test, dup_shares_offset) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t setup = -1;
    ASSERT_EQ(resource::open(task, "/dup_offsets", fs::O_CREAT | fs::O_RDWR, &setup), resource::OK);
    ASSERT_EQ(resource::write(task, setup, "abc", 3), static_cast<ssize_t>(3));
    ASSERT_EQ(resource::close(task, setup), resource::OK);

    resource::handle_t h1 = -1;
    ASSERT_EQ(resource::open(task, "/dup_offsets", fs::O_RDONLY, &h1), resource::OK);

    int64_t h2 = call_dup(h1);
    ASSERT_TRUE(h2 >= 0);

    char c1[2] = {};
    char c2[2] = {};
    ASSERT_EQ(resource::read(task, h1, c1, 1), static_cast<ssize_t>(1));
    ASSERT_EQ(resource::read(task, static_cast<resource::handle_t>(h2), c2, 1), static_cast<ssize_t>(1));
    EXPECT_STREQ(c1, "a");
    EXPECT_STREQ(c2, "b");

    ASSERT_EQ(resource::close(task, h1), resource::OK);
    ASSERT_EQ(resource::close(task, static_cast<resource::handle_t>(h2)), resource::OK);
}

TEST(resource_test, dup_returns_lowest_free_slot) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t a = -1;
    resource::handle_t b = -1;
    ASSERT_EQ(resource::open(task, "/dup_lowest_a", fs::O_CREAT | fs::O_RDWR, &a), resource::OK);
    ASSERT_EQ(resource::open(task, "/dup_lowest_b", fs::O_CREAT | fs::O_RDWR, &b), resource::OK);
    ASSERT_TRUE(a < b);

    ASSERT_EQ(resource::close(task, a), resource::OK);

    int64_t d = call_dup(b);
    EXPECT_EQ(d, static_cast<int64_t>(a));

    ASSERT_EQ(resource::close(task, b), resource::OK);
    ASSERT_EQ(resource::close(task, static_cast<resource::handle_t>(d)), resource::OK);
}

TEST(resource_test, dup2_replaces_and_closes_target) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    static const resource::resource_ops victim_ops = {
        nullptr, nullptr, close_counter_close, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr,
    };

    close_counter counter{0};

    auto* victim_obj = heap::kalloc_new<resource::resource_object>();
    ASSERT_NOT_NULL(victim_obj);
    victim_obj->type = resource::resource_type::FILE;
    victim_obj->ops = &victim_ops;
    victim_obj->impl = &counter;

    resource::handle_t victim = -1;
    ASSERT_EQ(
        resource::alloc_handle(&task->handles, victim_obj, resource::resource_type::FILE, resource::RIGHT_READ, &victim),
        resource::HANDLE_OK
    );
    resource::resource_release(victim_obj);

    resource::handle_t f = -1;
    ASSERT_EQ(resource::open(task, "/dup2_evict", fs::O_CREAT | fs::O_RDWR, &f), resource::OK);

    EXPECT_EQ(call_dup2(f, victim), static_cast<int64_t>(victim));
    EXPECT_EQ(counter.closes, 1u);

    ASSERT_EQ(resource::close(task, f), resource::OK);
    ASSERT_EQ(resource::close(task, victim), resource::OK);
}

TEST(resource_test, dup2_same_fd_is_validated_noop) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t f = -1;
    ASSERT_EQ(resource::open(task, "/dup2_same", fs::O_CREAT | fs::O_RDWR, &f), resource::OK);

    EXPECT_EQ(call_dup2(f, f), static_cast<int64_t>(f));
    EXPECT_EQ(call_dup2(f, static_cast<resource::handle_t>(resource::MAX_TASK_HANDLES)), syscall::EBADF);

    ASSERT_EQ(resource::close(task, f), resource::OK);

    EXPECT_EQ(call_dup2(f, f), syscall::EBADF);
    EXPECT_EQ(call_dup(f), syscall::EBADF);
}

TEST(resource_test, dup3_validates_flags_and_fds) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t f = -1;
    ASSERT_EQ(resource::open(task, "/dup3_validate", fs::O_CREAT | fs::O_RDWR, &f), resource::OK);

    EXPECT_EQ(call_dup3(f, f, 0), syscall::EINVAL);
    EXPECT_EQ(call_dup3(f, f + 1, fs::O_NONBLOCK), syscall::EINVAL);

    ASSERT_EQ(resource::close(task, f), resource::OK);
}

TEST(resource_test, dup_flag_inheritance_and_dup3_cloexec) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t f = -1;
    ASSERT_EQ(resource::open(task, "/dup_flags", fs::O_CREAT | fs::O_RDWR, &f), resource::OK);
    ASSERT_EQ(
        resource::set_handle_flags(&task->handles, f, resource::RESOURCE_HANDLE_CLOEXEC | fs::O_NONBLOCK),
        resource::HANDLE_OK
    );

    int64_t d = call_dup(f);
    ASSERT_TRUE(d >= 0);

    uint32_t dflags = 0;
    ASSERT_EQ(resource::get_handle_flags(&task->handles, static_cast<resource::handle_t>(d), &dflags), resource::HANDLE_OK);
    EXPECT_EQ(dflags, static_cast<uint32_t>(fs::O_NONBLOCK));

    ASSERT_EQ(resource::close(task, static_cast<resource::handle_t>(d)), resource::OK);

    int64_t t = call_dup3(f, static_cast<resource::handle_t>(d), fs::O_CLOEXEC);
    EXPECT_EQ(t, d);

    uint32_t tflags = 0;
    ASSERT_EQ(resource::get_handle_flags(&task->handles, static_cast<resource::handle_t>(t), &tflags), resource::HANDLE_OK);
    EXPECT_EQ(tflags, static_cast<uint32_t>(fs::O_NONBLOCK | resource::RESOURCE_HANDLE_CLOEXEC));

    ASSERT_EQ(resource::close(task, f), resource::OK);
    ASSERT_EQ(resource::close(task, static_cast<resource::handle_t>(t)), resource::OK);
}
