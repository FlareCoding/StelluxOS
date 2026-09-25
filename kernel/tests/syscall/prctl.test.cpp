#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_prctl.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "common/string.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(prctl_syscall);

constexpr uint64_t PR_SET_NAME       = 15;
constexpr uint64_t PR_GET_NAME       = 16;
constexpr uint64_t UNKNOWN_OPTION    = 0;
constexpr size_t   THREAD_NAME_BYTES = 16;
constexpr size_t   NAME_OUT_OFFSET   = 64; // Where PR_GET_NAME writes within the page

// Puts the calling task's name back when a test that renames it ends
struct name_restorer {
    char saved[sched::TASK_NAME_MAX];

    name_restorer() { string::memcpy(saved, sched::current()->name, sizeof(saved)); }
    ~name_restorer() { string::memcpy(sched::current()->name, saved, sizeof(saved)); }
};

static int64_t prctl_in(user_page& page, uint64_t option, uint64_t arg2) {
    user_space_scope scope(page.ctx);
    return sys_prctl(option, arg2, 0, 0, 0, 0);
}

static void put_string(user_page& page, size_t offset, const char* s) {
    string::memcpy(page.at<char>(offset), s, string::strlen(s) + 1);
}

TEST(prctl_syscall, set_name_then_get_name_round_trips) {
    name_restorer restore;
    user_page page;
    ASSERT_TRUE(page.ready());

    put_string(page, 0, "worker-7");
    ASSERT_EQ(prctl_in(page, PR_SET_NAME, page.addr), 0);
    EXPECT_STREQ(sched::current()->name, "worker-7");

    ASSERT_EQ(prctl_in(page, PR_GET_NAME, page.addr + NAME_OUT_OFFSET), 0);
    EXPECT_STREQ(page.at<char>(NAME_OUT_OFFSET), "worker-7");
}

TEST(prctl_syscall, long_names_are_cut_to_fifteen_characters) {
    name_restorer restore;
    user_page page;
    ASSERT_TRUE(page.ready());

    put_string(page, 0, "a-thread-name-past-sixteen-bytes");
    ASSERT_EQ(prctl_in(page, PR_SET_NAME, page.addr), 0);
    EXPECT_STREQ(sched::current()->name, "a-thread-name-p");
}

TEST(prctl_syscall, names_ending_at_the_edge_of_mapped_memory_are_read) {
    name_restorer restore;
    user_page page;
    ASSERT_TRUE(page.ready());

    // Fifteen characters and no terminator, the last one on the page's final byte
    size_t offset = pmm::PAGE_SIZE - (THREAD_NAME_BYTES - 1);
    string::memcpy(page.at<char>(offset), "fifteen-chars-x", THREAD_NAME_BYTES - 1);
    ASSERT_EQ(prctl_in(page, PR_SET_NAME, page.addr + offset), 0);
    EXPECT_STREQ(sched::current()->name, "fifteen-chars-x");
}

TEST(prctl_syscall, get_name_cuts_long_task_names_and_zero_pads) {
    name_restorer restore;
    user_page page;
    ASSERT_TRUE(page.ready());

    // Task names can outgrow the ABI buffer, so this one is set directly
    const char* long_name = "a-task-name-that-outgrows-sixteen-bytes";
    string::memcpy(sched::current()->name, long_name, string::strlen(long_name) + 1);
    string::memset(page.at<char>(0), 'x', THREAD_NAME_BYTES);

    ASSERT_EQ(prctl_in(page, PR_GET_NAME, page.addr), 0);
    EXPECT_STREQ(page.at<char>(0), "a-task-name-tha");

    string::memcpy(sched::current()->name, "abc", 4);
    ASSERT_EQ(prctl_in(page, PR_GET_NAME, page.addr), 0);
    for (size_t i = 3; i < THREAD_NAME_BYTES; i++) {
        EXPECT_EQ(page.at<char>(0)[i], '\0');
    }
}

TEST(prctl_syscall, unknown_options_are_refused) {
    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(prctl_in(page, UNKNOWN_OPTION, page.addr), syscall::EINVAL);
}

TEST(prctl_syscall, unmapped_name_buffers_fault) {
    name_restorer restore;
    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(prctl_in(page, PR_SET_NAME, 0), syscall::EFAULT);
    EXPECT_EQ(prctl_in(page, PR_GET_NAME, 0), syscall::EFAULT);
}
