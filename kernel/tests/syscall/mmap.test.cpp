#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_memfd.h"
#include "syscall/handlers/sys_mmap.h"
#include "syscall/syscall_table.h"
#include "resource/resource.h"
#include "resource/providers/shm_provider.h"
#include "fs/fs.h"
#include "fs/fstypes.h"
#include "sched/sched.h"

TEST_SUITE(mmap_syscall);

using test_helpers::user_page;
using test_helpers::user_space_scope;

static constexpr uint64_t PROT_NONE_ARG = 0x0;
static constexpr uint64_t PROT_READ_ARG = 0x1;
static constexpr uint64_t PROT_WRITE_ARG = 0x2;
static constexpr uint64_t PROT_READ_WRITE_ARG = 0x3;
static constexpr uint64_t PROT_EXEC_ARG = 0x4;
static constexpr uint64_t PROT_READ_EXEC_ARG = 0x5;
static constexpr uint64_t MAP_SHARED_ARG = 0x01;
static constexpr uint64_t MAP_PRIVATE_ARG = 0x02;
static constexpr uint64_t MAP_PRIVATE_ANONYMOUS_ARG = 0x22;
static constexpr uint64_t MAP_FIXED_ARG = 0x10;
static constexpr uint64_t NO_DESCRIPTOR_ARG = static_cast<uint64_t>(-1);
static constexpr uint64_t IGNORED_DESCRIPTOR_ARG = 0;

TEST(mmap_syscall, anonymous_mappings_ignore_the_descriptor) {
    user_page page;
    ASSERT_TRUE(page.ready());
    user_space_scope scope(page.ctx);

    int64_t reserved = sys_mmap(0, 2 * pmm::PAGE_SIZE, PROT_NONE_ARG, MAP_PRIVATE_ANONYMOUS_ARG,
                                NO_DESCRIPTOR_ARG, 0);
    ASSERT_TRUE(reserved > 0);

    uint64_t second_page = static_cast<uint64_t>(reserved) + pmm::PAGE_SIZE;
    EXPECT_EQ(sys_mmap(second_page, pmm::PAGE_SIZE, PROT_NONE_ARG, MAP_PRIVATE_ANONYMOUS_ARG | MAP_FIXED_ARG,
                       IGNORED_DESCRIPTOR_ARG, 0),
              static_cast<int64_t>(second_page));

    EXPECT_TRUE(sys_mmap(0, pmm::PAGE_SIZE, PROT_READ_WRITE_ARG, MAP_PRIVATE_ANONYMOUS_ARG,
                         IGNORED_DESCRIPTOR_ARG, 0) > 0);
}

static int64_t map_readable_page(uint64_t flags, resource::handle_t h) {
    return sys_mmap(0, pmm::PAGE_SIZE, PROT_READ_ARG, flags, static_cast<uint64_t>(h), 0);
}

static int64_t protect_one_page(int64_t addr, uint64_t prot) {
    return sys_mprotect(static_cast<uint64_t>(addr), pmm::PAGE_SIZE, prot, 0, 0, 0);
}

// Through a read-only handle a shared mapping never holds WRITE but a private copy can, a write-only handle maps
// nothing, and through a read-write one a shared mapping can hold WRITE
static void expect_handle_rights_to_cap_shared_mappings(const char* path) {
    sched::task* task = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());
    user_space_scope scope(page.ctx);

    resource::handle_t read_write = -1;
    ASSERT_EQ(resource::open(task, path, fs::O_CREAT | fs::O_RDWR, &read_write), resource::OK);
    ASSERT_EQ(sys_ftruncate(static_cast<uint64_t>(read_write), pmm::PAGE_SIZE, 0, 0, 0, 0), static_cast<int64_t>(0));

    resource::handle_t read_only = -1;
    ASSERT_EQ(resource::open(task, path, fs::O_RDONLY, &read_only), resource::OK);

    resource::handle_t write_only = -1;
    ASSERT_EQ(resource::open(task, path, fs::O_WRONLY, &write_only), resource::OK);

    EXPECT_EQ(sys_mmap(0, pmm::PAGE_SIZE, PROT_READ_WRITE_ARG, MAP_SHARED_ARG, static_cast<uint64_t>(read_only), 0),
              syscall::EACCES);
    EXPECT_EQ(map_readable_page(MAP_SHARED_ARG, write_only), syscall::EACCES);
    EXPECT_EQ(sys_mmap(0, pmm::PAGE_SIZE, PROT_EXEC_ARG, MAP_SHARED_ARG, static_cast<uint64_t>(write_only), 0),
              syscall::EACCES);
    EXPECT_EQ(sys_mmap(0, pmm::PAGE_SIZE, PROT_WRITE_ARG, MAP_SHARED_ARG, static_cast<uint64_t>(write_only), 0),
              syscall::EACCES);
    EXPECT_EQ(sys_mmap(0, pmm::PAGE_SIZE, PROT_NONE_ARG, MAP_SHARED_ARG, static_cast<uint64_t>(write_only), 0),
              syscall::EACCES);
    EXPECT_EQ(map_readable_page(MAP_PRIVATE_ARG, write_only), syscall::EACCES);

    int64_t shared = map_readable_page(MAP_SHARED_ARG, read_only);
    ASSERT_TRUE(shared > 0);
    EXPECT_EQ(protect_one_page(shared, PROT_READ_WRITE_ARG), syscall::EACCES);
    EXPECT_EQ(protect_one_page(shared, PROT_NONE_ARG), static_cast<int64_t>(0));
    EXPECT_EQ(protect_one_page(shared, PROT_READ_EXEC_ARG), static_cast<int64_t>(0));

    int64_t read_write_shared = map_readable_page(MAP_SHARED_ARG, read_write);
    ASSERT_TRUE(read_write_shared > 0);
    EXPECT_EQ(protect_one_page(read_write_shared, PROT_READ_WRITE_ARG), static_cast<int64_t>(0));

    int64_t private_copy = map_readable_page(MAP_PRIVATE_ARG, read_only);
    ASSERT_TRUE(private_copy > 0);
    EXPECT_EQ(protect_one_page(private_copy, PROT_READ_WRITE_ARG), static_cast<int64_t>(0));
    EXPECT_EQ(protect_one_page(private_copy, PROT_READ_EXEC_ARG), static_cast<int64_t>(0));
    EXPECT_TRUE(sys_mmap(0, pmm::PAGE_SIZE, PROT_READ_WRITE_ARG, MAP_PRIVATE_ARG,
                         static_cast<uint64_t>(read_only), 0) > 0);

    resource::close(task, write_only);
    resource::close(task, read_only);
    resource::close(task, read_write);
}

TEST(mmap_syscall, shared_memory_handle_rights_cap_shared_mappings) {
    expect_handle_rights_to_cap_shared_mappings("/dev/shm/mmap_rights");
    resource::shm_provider::unlink_shm("/dev/shm/mmap_rights");
}

TEST(mmap_syscall, file_handle_rights_cap_shared_mappings) {
    expect_handle_rights_to_cap_shared_mappings("/mmap_rights");
    fs::unlink("/mmap_rights");
}
