#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_dup.h"
#include "syscall/handlers/sys_fd.h"
#include "syscall/handlers/sys_memfd.h"
#include "syscall/handlers/sys_mmap.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/handle_table.h"
#include "fs/fstypes.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(handle_restrict);

using test_helpers::user_page;
using test_helpers::user_space_scope;

constexpr uint64_t F_GETFL = 3;
constexpr uint64_t PROT_READ_ARG = 0x1;
constexpr uint64_t PROT_READ_WRITE_ARG = 0x3;
constexpr uint64_t MAP_SHARED_ARG = 0x01;
constexpr uint64_t UNNAMED = 0;
constexpr uint64_t UNKNOWN_RIGHT = 1u << 2;
constexpr uint64_t CLOSED_HANDLE = 999;

static int64_t restrict_handle(resource::handle_t h, uint64_t rights) {
    int64_t result = 0;
    RUN_ELEVATED(result = sys_handle_restrict(static_cast<uint64_t>(h), rights, 0, 0, 0, 0));
    return result;
}

static resource::handle_t create_memfd() {
    int64_t h = 0;
    RUN_ELEVATED(h = sys_memfd_create(UNNAMED, 0, 0, 0, 0, 0));
    return static_cast<resource::handle_t>(h);
}

static int64_t access_mode_of(resource::handle_t h) {
    int64_t flags = 0;
    RUN_ELEVATED(flags = sys_fcntl(static_cast<uint64_t>(h), F_GETFL, 0, 0, 0, 0));
    return flags < 0 ? flags : (flags & (fs::O_WRONLY | fs::O_RDWR));
}

static resource::resource_object* object_of(sched::task* task, resource::handle_t h) {
    resource::resource_object* obj = nullptr;
    RUN_ELEVATED({
        if (resource::get_handle_object(task->handles, h, 0, &obj) == resource::HANDLE_OK) {
            resource::resource_release(obj);
        }
    });

    return obj;
}

TEST(handle_restrict, a_read_only_handle_reads_but_cannot_write_truncate_or_map_writable) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t writable = create_memfd();
    ASSERT_TRUE(writable >= 0);

    int64_t sized = 0;
    RUN_ELEVATED(sized = sys_ftruncate(static_cast<uint64_t>(writable), pmm::PAGE_SIZE, 0, 0, 0, 0));
    ASSERT_EQ(sized, static_cast<int64_t>(0));
    EXPECT_EQ(resource::write_at(task, writable, "stellux", 7, 0), static_cast<ssize_t>(7));

    int64_t restricted = restrict_handle(writable, resource::RIGHT_READ);
    ASSERT_TRUE(restricted >= 0);
    auto readable = static_cast<resource::handle_t>(restricted);

    EXPECT_NE(readable, writable);
    EXPECT_EQ(object_of(task, readable), object_of(task, writable));
    EXPECT_EQ(access_mode_of(readable), static_cast<int64_t>(fs::O_RDONLY));
    EXPECT_EQ(access_mode_of(writable), static_cast<int64_t>(fs::O_RDWR));

    EXPECT_EQ(resource::write(task, readable, "x", 1), static_cast<ssize_t>(resource::ERR_ACCESS));

    int64_t truncated = 0;
    int64_t allocated = 0;
    RUN_ELEVATED({
        truncated = sys_ftruncate(static_cast<uint64_t>(readable), 0, 0, 0, 0, 0);
        allocated = sys_fallocate(static_cast<uint64_t>(readable), 0, 0, pmm::PAGE_SIZE, 0, 0);
    });
    EXPECT_EQ(truncated, syscall::EACCES);
    EXPECT_EQ(allocated, syscall::EBADF);

    char text[8] = {};
    EXPECT_EQ(resource::read_at(task, readable, text, 7, 0), static_cast<ssize_t>(7));
    EXPECT_EQ(string::memcmp(text, "stellux", 7), 0);

    user_page page;
    ASSERT_TRUE(page.ready());

    int64_t writable_map = 0;
    int64_t readable_map = 0;
    {
        user_space_scope scope(page.ctx);
        RUN_ELEVATED({
            writable_map = sys_mmap(0, pmm::PAGE_SIZE, PROT_READ_WRITE_ARG, MAP_SHARED_ARG,
                                    static_cast<uint64_t>(readable), 0);
            readable_map = sys_mmap(0, pmm::PAGE_SIZE, PROT_READ_ARG, MAP_SHARED_ARG,
                                    static_cast<uint64_t>(readable), 0);
        });
    }

    EXPECT_EQ(writable_map, syscall::EACCES);
    EXPECT_TRUE(readable_map > 0);

    resource::close(task, readable);
    resource::close(task, writable);
}

TEST(handle_restrict, keeps_a_right_the_handle_holds_and_refuses_any_it_lacks) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t writable = create_memfd();
    ASSERT_TRUE(writable >= 0);

    int64_t write_only = restrict_handle(writable, resource::RIGHT_WRITE);
    ASSERT_TRUE(write_only >= 0);
    EXPECT_EQ(access_mode_of(static_cast<resource::handle_t>(write_only)), static_cast<int64_t>(fs::O_WRONLY));

    int64_t both = restrict_handle(writable, resource::RIGHT_MASK);
    ASSERT_TRUE(both >= 0);
    EXPECT_EQ(access_mode_of(static_cast<resource::handle_t>(both)), static_cast<int64_t>(fs::O_RDWR));

    int64_t readable = restrict_handle(writable, resource::RIGHT_READ);
    ASSERT_TRUE(readable >= 0);
    auto read_only = static_cast<resource::handle_t>(readable);
    EXPECT_EQ(restrict_handle(read_only, resource::RIGHT_MASK), syscall::EINVAL);
    EXPECT_EQ(restrict_handle(read_only, resource::RIGHT_WRITE), syscall::EINVAL);
    EXPECT_EQ(restrict_handle(static_cast<resource::handle_t>(write_only), resource::RIGHT_READ), syscall::EINVAL);

    resource::close(task, read_only);
    resource::close(task, static_cast<resource::handle_t>(both));
    resource::close(task, static_cast<resource::handle_t>(write_only));
    resource::close(task, writable);
}

TEST(handle_restrict, refuses_no_rights_an_unknown_right_and_a_bad_handle) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t writable = create_memfd();
    ASSERT_TRUE(writable >= 0);

    EXPECT_EQ(restrict_handle(writable, 0), syscall::EINVAL);
    EXPECT_EQ(restrict_handle(writable, UNKNOWN_RIGHT), syscall::EINVAL);
    EXPECT_EQ(restrict_handle(writable, resource::RIGHT_READ | UNKNOWN_RIGHT), syscall::EINVAL);
    EXPECT_EQ(restrict_handle(static_cast<resource::handle_t>(CLOSED_HANDLE), resource::RIGHT_READ), syscall::EBADF);
    EXPECT_EQ(restrict_handle(-1, resource::RIGHT_READ), syscall::EBADF);

    resource::close(task, writable);
}

TEST(handle_restrict, the_new_handle_takes_the_lowest_free_slot_without_close_on_exec) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    resource::handle_t writable = create_memfd();
    ASSERT_TRUE(writable >= 0);
    ASSERT_EQ(resource::set_handle_flags(task->handles, writable, resource::RESOURCE_HANDLE_CLOEXEC),
              resource::HANDLE_OK);

    resource::handle_t placeholder = create_memfd();
    ASSERT_TRUE(placeholder >= 0);
    resource::close(task, placeholder);

    int64_t restricted = restrict_handle(writable, resource::RIGHT_READ);
    EXPECT_EQ(restricted, static_cast<int64_t>(placeholder));

    uint32_t flags = 0;
    EXPECT_EQ(resource::get_handle_flags(task->handles, static_cast<resource::handle_t>(restricted), &flags),
              resource::HANDLE_OK);
    EXPECT_EQ(flags & resource::RESOURCE_HANDLE_CLOEXEC, 0u);

    resource::close(task, static_cast<resource::handle_t>(restricted));
    resource::close(task, writable);
}
