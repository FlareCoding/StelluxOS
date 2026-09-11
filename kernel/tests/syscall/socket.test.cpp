#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_socket.h"
#include "resource/resource.h"
#include "net/inet.h"
#include "fs/fstypes.h"
#include "sched/sched.h"
#include "sched/task.h"

TEST_SUITE(socket_syscall);

static uint32_t handle_flags_of(sched::task* task, int64_t h) {
    resource::resource_object* obj = nullptr;
    uint32_t flags = 0;
    if (resource::get_handle_object(task->handles, static_cast<resource::handle_t>(h),
                                    resource::RIGHT_READ, &obj, &flags) == resource::HANDLE_OK) {
        resource::resource_release(obj);
    }

    return flags;
}

TEST(socket_syscall, creation_flags_land_on_the_handle) {
    sched::task* task = sched::current();
    ASSERT_NOT_NULL(task);

    int64_t plain = sys_socket(net::inet::AF_INET, net::inet::SOCK_DGRAM, 0, 0, 0, 0);
    int64_t flagged = sys_socket(net::inet::AF_INET,
                                 net::inet::SOCK_DGRAM | fs::O_NONBLOCK | fs::O_CLOEXEC, 0, 0, 0, 0);
    ASSERT_TRUE(plain >= 0);
    ASSERT_TRUE(flagged >= 0);

    EXPECT_EQ(handle_flags_of(task, plain), 0u);
    EXPECT_EQ(handle_flags_of(task, flagged), fs::O_NONBLOCK | resource::RESOURCE_HANDLE_CLOEXEC);

    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(plain)), resource::OK);
    EXPECT_EQ(resource::close(task, static_cast<resource::handle_t>(flagged)), resource::OK);
}

TEST(socket_syscall, unknown_type_bits_are_still_rejected) {
    int64_t rc = sys_socket(net::inet::AF_INET, net::inet::SOCK_DGRAM | 0x10, 0, 0, 0, 0);
    EXPECT_EQ(rc, syscall::EPROTONOSUPPORT);
}
