#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "syscall/handlers/sys_pipe.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "mm/heap.h"
#include "fs/fstypes.h"
#include "clock/clock.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(process_exit);

using test_helpers::user_page;
using test_helpers::user_space_scope;

static constexpr uint64_t KILL_POLL_MS = 10;

static void exit_at_once(void*) {
    sched::exit(0);
}

static void sleep_until_killed(void*) {
    RUN_ELEVATED({
        while (!sched::block_task_interrupted()) {
            sched::sleep_ms(KILL_POLL_MS);
        }
    });

    sched::exit(0);
}

static int64_t create_nonblocking_pipe_into(user_page& page) {
    user_space_scope scope(page.ctx);
    return sys_pipe2(page.addr, fs::O_NONBLOCK, 0, 0, 0, 0);
}

// A hand-built process whose only thread besides `leader` is `member`
static sched::thread_group* build_process(sched::task* leader, sched::task* member) {
    sched::thread_group* group = nullptr;
    RUN_ELEVATED({
        group = heap::kalloc_new<sched::thread_group>();
        if (group) {
            group->lock = sync::SPINLOCK_INIT;
            group->threads.init();
            group->leader = leader;
            group->threads.push_back(member);
            group->thread_count = 1;

            // One reference for each task, which the reaper releases
            group->add_ref();
            group->add_ref();
            leader->group = group;
            member->group = group;
        }
    });

    return group;
}

static bool give_write_end(sched::task* t, resource::resource_object* write_end) {
    int32_t rc = resource::HANDLE_ERR_INVAL;
    RUN_ELEVATED({
        resource::handle_t installed = -1;
        rc = resource::alloc_task_handle(t, write_end, resource::resource_type::PIPE,
                                         resource::RIGHT_WRITE, &installed);
    });

    return rc == resource::HANDLE_OK;
}

static bool wait_until_dead(sched::task* t) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (t->state.load_acquire() != sched::TASK_STATE_DEAD) {
        if (clock::now_ns() > deadline) {
            return false;
        }
    }

    return true;
}

TEST(process_exit, the_leader_finishes_exiting_only_after_its_threads_released_their_handles) {
    sched::task* self = sched::current();
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(create_nonblocking_pipe_into(page), static_cast<int64_t>(0));
    int32_t read_end = page.at<int32_t>(0)[0];
    int32_t write_end = page.at<int32_t>(0)[1];

    resource::resource_object* write_end_obj = nullptr;
    ASSERT_EQ(resource::get_handle_object(self->handles, write_end, 0, &write_end_obj), resource::HANDLE_OK);

    rc::strong_ref<sched::task> leader;
    rc::strong_ref<sched::task> member;
    RUN_ELEVATED({
        leader = sched::task_ref(sched::create_kernel_task(exit_at_once, nullptr, "test_exit_leader"));
        member = sched::task_ref(sched::create_kernel_task(sleep_until_killed, nullptr, "test_exit_member"));
    });

    ASSERT_TRUE(static_cast<bool>(leader));
    ASSERT_TRUE(static_cast<bool>(member));

    // Given before the tasks join the group, whose handle limit is zero in a hand-built process
    ASSERT_TRUE(give_write_end(leader.ptr(), write_end_obj));
    ASSERT_TRUE(give_write_end(member.ptr(), write_end_obj));
    resource::resource_release(write_end_obj);
    resource::close(self, write_end);

    sched::thread_group* group = build_process(leader.ptr(), member.ptr());
    ASSERT_NOT_NULL(group);

    RUN_ELEVATED({ sched::enqueue(member.ptr()); });
    ASSERT_TRUE(test_helpers::blocks_before_deadline(member.ptr()));

    RUN_ELEVATED({ sched::enqueue(leader.ptr()); });
    ASSERT_TRUE(wait_until_dead(leader.ptr()));

    EXPECT_TRUE(leader->handles == nullptr);
    EXPECT_TRUE(member->handles == nullptr);

    uint8_t byte = 0;
    EXPECT_EQ(resource::read(self, read_end, &byte, sizeof(byte)), static_cast<ssize_t>(0));

    resource::close(self, read_end);
    RUN_ELEVATED({
        leader.reset();
        member.reset();
        if (group->release()) {
            sched::thread_group::ref_destroy(group);
        }
    });
}
