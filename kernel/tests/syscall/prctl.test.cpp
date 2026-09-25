#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_prctl.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/spinlock.h"
#include "mm/heap.h"
#include "common/string.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(prctl_syscall);

constexpr uint64_t PR_GET_DUMPABLE     = 3;
constexpr uint64_t PR_SET_DUMPABLE     = 4;
constexpr uint64_t PR_SET_NAME         = 15;
constexpr uint64_t PR_GET_NAME         = 16;
constexpr uint64_t PR_SET_TIMERSLACK   = 29;
constexpr uint64_t PR_GET_TIMERSLACK   = 30;
constexpr uint64_t PR_SET_NO_NEW_PRIVS = 38;
constexpr uint64_t PR_GET_NO_NEW_PRIVS = 39;
constexpr uint64_t UNKNOWN_OPTION      = 0;
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

static int64_t prctl_call(uint64_t option, uint64_t arg2, uint64_t arg3 = 0, uint64_t arg4 = 0,
                          uint64_t arg5 = 0) {
    return sys_prctl(option, arg2, arg3, arg4, arg5, 0);
}

// Lends the calling kernel task a process for the length of a test, since the
// process-wide options live on its thread group
struct borrowed_group {
    sched::thread_group* saved = sched::current()->group;
    sched::thread_group* group = heap::kalloc_new<sched::thread_group>();

    borrowed_group() {
        if (group) {
            group->lock = sync::SPINLOCK_INIT;
            sched::current()->group = group;
        }
    }

    ~borrowed_group() {
        sched::current()->group = saved;
        if (group) {
            heap::kfree_delete(group);
        }
    }
};

// Puts the calling task's per-thread prctl state back when a test ends. The
// test clears no-new-privileges directly, since the syscall can only set it.
struct thread_state_restorer {
    uint64_t slack = sched::current()->timer_slack_ns;
    uint64_t default_slack = sched::current()->default_timer_slack_ns;
    bool no_new_privs = sched::current()->no_new_privs;

    ~thread_state_restorer() {
        sched::task* self = sched::current();
        self->timer_slack_ns = slack;
        self->default_timer_slack_ns = default_slack;
        self->no_new_privs = no_new_privs;
    }
};

TEST(prctl_syscall, dumpable_round_trips_on_the_process) {
    borrowed_group process;
    ASSERT_NOT_NULL(process.group);

    EXPECT_EQ(prctl_call(PR_GET_DUMPABLE, 0), 1);
    EXPECT_EQ(prctl_call(PR_SET_DUMPABLE, 0), 0);
    EXPECT_EQ(prctl_call(PR_GET_DUMPABLE, 0), 0);
    EXPECT_EQ(prctl_call(PR_SET_DUMPABLE, 1), 0);
    EXPECT_EQ(prctl_call(PR_GET_DUMPABLE, 0), 1);
}

TEST(prctl_syscall, dumpable_refuses_reserved_values_and_callers_without_a_process) {
    {
        borrowed_group process;
        ASSERT_NOT_NULL(process.group);

        EXPECT_EQ(prctl_call(PR_SET_DUMPABLE, 2), syscall::EINVAL);
        EXPECT_EQ(prctl_call(PR_GET_DUMPABLE, 0), 1);
    }

    EXPECT_EQ(prctl_call(PR_GET_DUMPABLE, 0), syscall::ESRCH);
    EXPECT_EQ(prctl_call(PR_SET_DUMPABLE, 0), syscall::ESRCH);
}

TEST(prctl_syscall, timer_slack_round_trips_and_zero_restores_the_default) {
    thread_state_restorer restore;
    int64_t start = static_cast<int64_t>(sched::current()->default_timer_slack_ns);

    EXPECT_EQ(prctl_call(PR_SET_TIMERSLACK, 1000), 0);
    EXPECT_EQ(prctl_call(PR_GET_TIMERSLACK, 0), 1000);
    EXPECT_EQ(prctl_call(PR_SET_TIMERSLACK, 0), 0);
    EXPECT_EQ(prctl_call(PR_GET_TIMERSLACK, 0), start);
}

TEST(prctl_syscall, no_new_privs_switches_on_and_stays_on) {
    thread_state_restorer restore;
    sched::current()->no_new_privs = false;

    EXPECT_EQ(prctl_call(PR_GET_NO_NEW_PRIVS, 0), 0);
    EXPECT_EQ(prctl_call(PR_SET_NO_NEW_PRIVS, 1), 0);
    EXPECT_EQ(prctl_call(PR_GET_NO_NEW_PRIVS, 0), 1);
    EXPECT_EQ(prctl_call(PR_SET_NO_NEW_PRIVS, 1), 0);
    EXPECT_EQ(prctl_call(PR_GET_NO_NEW_PRIVS, 0), 1);
}

TEST(prctl_syscall, no_new_privs_refuses_stray_arguments) {
    thread_state_restorer restore;
    sched::current()->no_new_privs = false;

    EXPECT_EQ(prctl_call(PR_SET_NO_NEW_PRIVS, 0), syscall::EINVAL);
    EXPECT_EQ(prctl_call(PR_SET_NO_NEW_PRIVS, 1, 1), syscall::EINVAL);
    EXPECT_EQ(prctl_call(PR_GET_NO_NEW_PRIVS, 1), syscall::EINVAL);
    EXPECT_EQ(prctl_call(PR_GET_NO_NEW_PRIVS, 0, 0, 1), syscall::EINVAL);
    EXPECT_EQ(prctl_call(PR_GET_NO_NEW_PRIVS, 0), 0);
}
