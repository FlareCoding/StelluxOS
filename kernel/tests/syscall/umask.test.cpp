#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_umask.h"
#include "syscall/handlers/sys_fd.h"
#include "resource/resource.h"
#include "fs/fs.h"
#include "sched/sched.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(umask_syscall);

using test_helpers::user_page;
using test_helpers::user_space_scope;

static constexpr uint64_t AT_FDCWD_ARG = static_cast<uint64_t>(-100);

static int64_t mkdirat_with_page_path(user_page& page, uint64_t mode) {
    user_space_scope scope(page.ctx);
    return sys_mkdirat(AT_FDCWD_ARG, page.addr, mode, 0, 0, 0);
}

static int64_t openat_with_page_path(user_page& page, uint64_t flags, uint64_t mode) {
    user_space_scope scope(page.ctx);
    return sys_openat(AT_FDCWD_ARG, page.addr, flags, mode, 0, 0);
}

// Swap semantics are verified live from userland, since kernel test
// tasks have no thread group to hold the mask.

TEST(umask_syscall, groupless_caller_is_esrch) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_umask(022, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);
}

static const char MASKED_DIRECTORY_PATH[] = "/umask_directory";

TEST(umask_syscall, mkdirat_clears_the_masked_bits) {
    user_page page;
    ASSERT_TRUE(page.ready());
    string::memcpy(page.bytes, MASKED_DIRECTORY_PATH, sizeof(MASKED_DIRECTORY_PATH));

    ASSERT_EQ(mkdirat_with_page_path(page, 0777), static_cast<int64_t>(0));

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(MASKED_DIRECTORY_PATH, &attr), fs::OK);
    EXPECT_EQ(attr.mode, 0777 & ~sched::current_umask());

    fs::rmdir(MASKED_DIRECTORY_PATH);
}

static const char MASKED_FILE_PATH[] = "/umask_file";

TEST(umask_syscall, openat_creates_files_without_the_masked_bits) {
    user_page page;
    ASSERT_TRUE(page.ready());
    string::memcpy(page.bytes, MASKED_FILE_PATH, sizeof(MASKED_FILE_PATH));

    int64_t fd = openat_with_page_path(page, fs::O_CREAT | fs::O_WRONLY, 0666);
    ASSERT_TRUE(fd >= 0);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(MASKED_FILE_PATH, &attr), fs::OK);
    EXPECT_EQ(attr.mode, 0666 & ~sched::current_umask());

    resource::close(sched::current(), static_cast<resource::handle_t>(fd));
    fs::unlink(MASKED_FILE_PATH);
}
