#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_fd.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(utimensat_syscall);

static constexpr uint64_t AT_FDCWD_ARG = static_cast<uint64_t>(-100);
static constexpr uint64_t AT_SYMLINK_NOFOLLOW_ARG = 0x100;

// Timestamp application is verified live from userland, since kernel test
// tasks have no user address space to hold the path or the times array.

TEST(utimensat_syscall, rejects_unknown_flags) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_utimensat(AT_FDCWD_ARG, 0, 0, 0x1, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);
}

TEST(utimensat_syscall, rejects_null_path_relative_to_cwd) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_utimensat(AT_FDCWD_ARG, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EFAULT);
}

TEST(utimensat_syscall, rejects_flags_on_descriptor_form) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_utimensat(3, 0, 0, AT_SYMLINK_NOFOLLOW_ARG, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);
}

TEST(utimensat_syscall, rejects_bad_descriptor) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_utimensat(9999, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EBADF);
}

TEST(utimensat_syscall, rejects_unreadable_times_pointer) {
    int64_t times = 0;
    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_utimensat(AT_FDCWD_ARG, 0, reinterpret_cast<uint64_t>(&times), 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::EFAULT);
}

TEST(utimensat_syscall, rejects_unreadable_path_pointer) {
    const char* path = "/";
    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_utimensat(AT_FDCWD_ARG, reinterpret_cast<uint64_t>(path), 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::EFAULT);
}
