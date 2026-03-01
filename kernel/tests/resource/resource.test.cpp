#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
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
