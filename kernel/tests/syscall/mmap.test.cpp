#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_mmap.h"

TEST_SUITE(mmap_syscall);

using test_helpers::user_page;
using test_helpers::user_space_scope;

static constexpr uint64_t PROT_NONE_ARG = 0x0;
static constexpr uint64_t PROT_READ_WRITE_ARG = 0x3;
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
