#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_fd.h"
#include "resource/resource.h"
#include "fs/fs.h"
#include "sched/sched.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(fchmodat_syscall);

using test_helpers::user_page;
using test_helpers::user_space_scope;

static constexpr uint64_t AT_FDCWD_ARG = static_cast<uint64_t>(-100);

static int64_t fchmodat_with_page_path(user_page& page, uint64_t mode) {
    user_space_scope scope(page.ctx);
    return sys_fchmodat(AT_FDCWD_ARG, page.addr, mode, 0, 0, 0);
}

TEST(fchmodat_syscall, rejects_unreadable_path_pointer) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_fchmodat(0, 0, 0644, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EFAULT);
}

static const char NAMED_FILE_PATH[] = "/fchmodat_named";

TEST(fchmodat_syscall, sets_the_mode_of_the_named_file) {
    fs::file* f = fs::open(NAMED_FILE_PATH, fs::O_CREAT | fs::O_RDWR);
    ASSERT_NOT_NULL(f);
    fs::close(f);

    user_page page;
    ASSERT_TRUE(page.ready());
    string::memcpy(page.bytes, NAMED_FILE_PATH, sizeof(NAMED_FILE_PATH));

    EXPECT_EQ(fchmodat_with_page_path(page, 0600), static_cast<int64_t>(0));

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(NAMED_FILE_PATH, &attr), fs::OK);
    EXPECT_EQ(attr.mode, static_cast<uint32_t>(0600));

    fs::unlink(NAMED_FILE_PATH);
}

static const char OPEN_FILE_PATH[] = "/fchmod_open";

TEST(fchmodat_syscall, fchmod_sets_the_mode_of_an_open_file) {
    sched::task* task = sched::current();
    resource::handle_t h = -1;
    ASSERT_EQ(resource::open(task, OPEN_FILE_PATH, fs::O_CREAT | fs::O_RDWR, &h), resource::OK);

    EXPECT_EQ(sys_fchmod(static_cast<uint64_t>(h), 0700, 0, 0, 0, 0), static_cast<int64_t>(0));

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(OPEN_FILE_PATH, &attr), fs::OK);
    EXPECT_EQ(attr.mode, static_cast<uint32_t>(0700));

    resource::close(task, h);
    fs::unlink(OPEN_FILE_PATH);
}
