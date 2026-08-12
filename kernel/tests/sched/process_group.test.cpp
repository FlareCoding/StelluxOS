#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_task.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "resource/handle_table.h"
#include "resource/resource.h"
#include "resource/providers/proc_provider.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(process_group);

// Two hand-built processes registered under reserved high tids
static sched::task* g_proc;
static sched::thread_group* g_tg;
static sched::task* g_proc2;
static sched::thread_group* g_tg2;

// Temporary process identity for the kernel test task itself
static sched::thread_group* g_self_tg;

constexpr uint32_t TEST_TID  = 0x00FF0001;
constexpr uint32_t TEST_TID2 = 0x00FF0002;

static void init_process(sched::task* t, sched::thread_group* tg, uint32_t tid) {
    tg->lock = sync::SPINLOCK_INIT;
    tg->leader = t;
    tg->pid = tid;
    tg->group_id = tid;
    tg->threads.init();
    tg->thread_count = 0;

    t->tid = tid;
    t->state = sched::TASK_STATE_CREATED;
    t->group = tg;
}

static int32_t setup_process() {
    RUN_ELEVATED({
        g_proc  = heap::kalloc_new<sched::task>();
        g_tg    = heap::kalloc_new<sched::thread_group>();
        g_proc2 = heap::kalloc_new<sched::task>();
        g_tg2   = heap::kalloc_new<sched::thread_group>();
    });
    if (!g_proc || !g_tg || !g_proc2 || !g_tg2) {
        return -1;
    }

    init_process(g_proc, g_tg, TEST_TID);
    init_process(g_proc2, g_tg2, TEST_TID2);

    RUN_ELEVATED({
        sched::g_task_registry.insert(g_proc);
        sched::g_task_registry.insert(g_proc2);
    });
    return 0;
}

// Give the kernel test task a process identity so the setpgid and
// getpgid self paths become reachable
static void attach_self_group() {
    sched::task* self = sched::current();
    RUN_ELEVATED({ g_self_tg = heap::kalloc_new<sched::thread_group>(); });

    g_self_tg->lock = sync::SPINLOCK_INIT;
    g_self_tg->leader = self;
    g_self_tg->pid = self->tid;
    g_self_tg->group_id = self->tid;
    g_self_tg->threads.init();
    g_self_tg->thread_count = 0;

    self->group = g_self_tg;
}

static void detach_self_group() {
    if (!g_self_tg) {
        return;
    }
    sched::current()->group = nullptr;
    RUN_ELEVATED({ heap::kfree_delete(g_self_tg); });
    g_self_tg = nullptr;
}

static int32_t teardown_process() {
    detach_self_group();
    RUN_ELEVATED({
        if (g_proc) {
            sched::g_task_registry.remove(*g_proc);
            heap::kfree_delete(g_proc);
        }
        if (g_proc2) {
            sched::g_task_registry.remove(*g_proc2);
            heap::kfree_delete(g_proc2);
        }
        if (g_tg) heap::kfree_delete(g_tg);
        if (g_tg2) heap::kfree_delete(g_tg2);
    });
    g_proc = nullptr;
    g_tg = nullptr;
    g_proc2 = nullptr;
    g_tg2 = nullptr;
    return 0;
}

BEFORE_EACH(process_group, setup_process);
AFTER_EACH(process_group, teardown_process);

TEST(process_group, getpgid_resolves_registered_process) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_getpgid(TEST_TID, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, static_cast<int64_t>(TEST_TID));
}

TEST(process_group, getpgid_unknown_pid_is_esrch) {
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_getpgid(0x00EE0000, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);

    // Values beyond the task id range can never name a process
    RUN_ELEVATED({ rc = sys_getpgid(0x100000000ull + TEST_TID, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);
}

TEST(process_group, kernel_task_is_not_a_process) {
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_getpgid(0, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);

    RUN_ELEVATED({ rc = sys_setpgid(0, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);
}

TEST(process_group, setpgid_self_creates_and_joins) {
    attach_self_group();
    uint32_t self_tid = sched::current()->tid;
    int64_t rc = 0;

    // Making yourself your own group is always allowed
    RUN_ELEVATED({ rc = sys_setpgid(0, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_self_tg->group_id, self_tid);

    // Joining an existing group with a live member
    RUN_ELEVATED({ rc = sys_setpgid(0, TEST_TID, 0, 0, 0, 0); });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_self_tg->group_id, TEST_TID);

    detach_self_group();
}

TEST(process_group, setpgid_join_requires_live_member) {
    attach_self_group();
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_setpgid(0, 0x00EE0000, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EPERM);

    detach_self_group();
}

TEST(process_group, setpgid_foreign_process_is_esrch) {
    attach_self_group();
    int64_t rc = 0;

    // The registered process is not a child the caller holds a handle to
    RUN_ELEVATED({ rc = sys_setpgid(TEST_TID, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);

    detach_self_group();
}

TEST(process_group, setpgid_regroups_unstarted_child) {
    attach_self_group();
    sched::task* self = sched::current();

    resource::resource_object* obj = nullptr;
    resource::handle_t handle = -1;
    int32_t rc32 = 0;
    RUN_ELEVATED({
        rc32 = resource::proc_provider::create_proc_resource(g_proc, &obj);
        if (rc32 == 0) {
            rc32 = resource::alloc_handle(
                &self->handles, obj, resource::resource_type::PROCESS, 0, &handle);
        }
    });
    ASSERT_EQ(rc32, 0);

    int64_t rc = 0;

    // An unstarted child owned via a handle may be moved into a live group
    RUN_ELEVATED({ rc = sys_setpgid(TEST_TID, TEST_TID2, 0, 0, 0, 0); });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_tg->group_id, TEST_TID2);

    // A started child may not be re-grouped anymore
    g_proc->state = sched::TASK_STATE_READY;
    RUN_ELEVATED({ rc = sys_setpgid(TEST_TID, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EACCES);
    g_proc->state = sched::TASK_STATE_CREATED;

    // Detach the hand-built child before dropping the proc resource so
    // its close path cannot run real task teardown on it
    RUN_ELEVATED({
        auto* pr = resource::proc_provider::get_proc_resource(obj);
        sync::irq_state irq = sync::spin_lock_irqsave(pr->lock);
        pr->child = nullptr;
        sync::spin_unlock_irqrestore(pr->lock, irq);

        if (g_proc->proc_res) {
            if (g_proc->proc_res->release()) {
                resource::proc_provider::proc_resource::ref_destroy(g_proc->proc_res);
            }
            g_proc->proc_res = nullptr;
        }

        resource::resource_object* removed = nullptr;
        if (resource::remove_handle(&self->handles, handle, &removed) == resource::HANDLE_OK) {
            resource::resource_release(removed);
        }
        resource::resource_release(obj);
    });

    detach_self_group();
}
