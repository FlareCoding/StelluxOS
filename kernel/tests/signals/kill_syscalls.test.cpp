#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "syscall/handlers/sys_signal.h"
#include "signals/signal.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(kill_syscalls);

// Two hand-built registered processes, a thread in the first one,
// and a kernel task, all under reserved high tids
static sched::task* g_proc;
static sched::task* g_thread;
static sched::thread_group* g_tg;
static sched::task* g_proc2;
static sched::thread_group* g_tg2;
static sched::task* g_ktask;

constexpr uint32_t TEST_TID   = 0x00FE0001;
constexpr uint32_t THREAD_TID = 0x00FE0002;
constexpr uint32_t TEST_TID2  = 0x00FE0003;
constexpr uint32_t KTASK_TID  = 0x00FE0004;

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

static int32_t setup_tasks() {
    RUN_ELEVATED({
        g_proc   = heap::kalloc_new<sched::task>();
        g_thread = heap::kalloc_new<sched::task>();
        g_tg     = heap::kalloc_new<sched::thread_group>();
        g_proc2  = heap::kalloc_new<sched::task>();
        g_tg2    = heap::kalloc_new<sched::thread_group>();
        g_ktask  = heap::kalloc_new<sched::task>();
    });
    if (!g_proc || !g_thread || !g_tg || !g_proc2 || !g_tg2 || !g_ktask) {
        return -1;
    }

    init_process(g_proc, g_tg, TEST_TID);
    init_process(g_proc2, g_tg2, TEST_TID2);

    g_thread->tid = THREAD_TID;
    g_thread->state = sched::TASK_STATE_CREATED;
    g_thread->group = g_tg;
    g_tg->threads.push_back(g_thread);
    g_tg->thread_count = 1;

    g_ktask->tid = KTASK_TID;
    g_ktask->state = sched::TASK_STATE_CREATED;
    g_ktask->exec.flags = sched::TASK_FLAG_KERNEL;

    RUN_ELEVATED({
        sched::g_task_registry.insert(g_proc);
        sched::g_task_registry.insert(g_thread);
        sched::g_task_registry.insert(g_proc2);
        sched::g_task_registry.insert(g_ktask);
    });
    return 0;
}

static int32_t teardown_tasks() {
    RUN_ELEVATED({
        sched::g_task_registry.remove(*g_proc);
        sched::g_task_registry.remove(*g_thread);
        sched::g_task_registry.remove(*g_proc2);
        sched::g_task_registry.remove(*g_ktask);
        heap::kfree_delete(g_proc);
        heap::kfree_delete(g_thread);
        heap::kfree_delete(g_proc2);
        heap::kfree_delete(g_ktask);
        heap::kfree_delete(g_tg);
        heap::kfree_delete(g_tg2);
    });
    g_proc = nullptr;
    g_thread = nullptr;
    g_tg = nullptr;
    g_proc2 = nullptr;
    g_tg2 = nullptr;
    g_ktask = nullptr;
    return 0;
}

BEFORE_EACH(kill_syscalls, setup_tasks);
AFTER_EACH(kill_syscalls, teardown_tasks);

TEST(kill_syscalls, rejects_invalid_signals) {
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_kill(TEST_TID, 65, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);

    RUN_ELEVATED({ rc = sys_tkill(THREAD_TID, 65, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);

    RUN_ELEVATED({ rc = sys_tkill(0, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);

    RUN_ELEVATED({ rc = sys_tgkill(0, THREAD_TID, signals::SIGTERM, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EINVAL);
}

TEST(kill_syscalls, broadcast_kill_is_unsupported) {
    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_kill(static_cast<uint64_t>(-1ll), signals::SIGTERM, 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::EINVAL);
}

TEST(kill_syscalls, unknown_targets_are_esrch) {
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_kill(0x00EE0000, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);

    RUN_ELEVATED({
        rc = sys_kill(static_cast<uint64_t>(-0x00EE0000ll), signals::SIGTERM,
                      0, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::ESRCH);

    RUN_ELEVATED({ rc = sys_tkill(0x00EE0000, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);
}

TEST(kill_syscalls, null_signal_probes_without_sending) {
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_kill(TEST_TID, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, 0);

    RUN_ELEVATED({
        rc = sys_kill(static_cast<uint64_t>(-static_cast<int64_t>(TEST_TID)),
                      0, 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, 0);

    // The probe reports the same permission gate a real send would
    RUN_ELEVATED({ rc = sys_tkill(KTASK_TID, 0, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EPERM);

    EXPECT_EQ(g_proc->sig.pending, 0ULL);
    EXPECT_EQ(g_tg->sig.shared_pending, 0ULL);
}

TEST(kill_syscalls, kill_signals_the_whole_process) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_kill(TEST_TID, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_tg->sig.shared_pending, signals::sig_bit(signals::SIGTERM));
}

TEST(kill_syscalls, kill_accepts_nonleader_thread_ids) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_kill(THREAD_TID, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_tg->sig.shared_pending, signals::sig_bit(signals::SIGTERM));
}

TEST(kill_syscalls, group_kill_reaches_every_member_process) {
    // Both processes join one group, addressed by negative pid
    g_tg2->group_id = TEST_TID;

    int64_t rc = 0;
    RUN_ELEVATED({
        rc = sys_kill(static_cast<uint64_t>(-static_cast<int64_t>(TEST_TID)),
                      signals::SIGTERM, 0, 0, 0, 0);
    });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_tg->sig.shared_pending, signals::sig_bit(signals::SIGTERM));
    EXPECT_EQ(g_tg2->sig.shared_pending, signals::sig_bit(signals::SIGTERM));
}

TEST(kill_syscalls, tkill_is_thread_directed) {
    int64_t rc = 0;
    RUN_ELEVATED({ rc = sys_tkill(THREAD_TID, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_thread->sig.pending, signals::sig_bit(signals::SIGTERM));
    EXPECT_EQ(g_tg->sig.shared_pending, 0ULL);
}

TEST(kill_syscalls, tgkill_validates_the_pair) {
    int64_t rc = 0;

    RUN_ELEVATED({
        rc = sys_tgkill(TEST_TID, THREAD_TID, signals::SIGTERM, 0, 0, 0);
    });
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(g_thread->sig.pending, signals::sig_bit(signals::SIGTERM));

    // A thread paired with the wrong process is not a match
    RUN_ELEVATED({
        rc = sys_tgkill(TEST_TID2, THREAD_TID, signals::SIGTERM, 0, 0, 0);
    });
    EXPECT_EQ(rc, syscall::ESRCH);
}

TEST(kill_syscalls, kernel_tasks_are_protected) {
    int64_t rc = 0;

    RUN_ELEVATED({ rc = sys_tkill(KTASK_TID, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::EPERM);

    // Without a thread group a kernel task is not a killable process
    RUN_ELEVATED({ rc = sys_kill(KTASK_TID, signals::SIGTERM, 0, 0, 0, 0); });
    EXPECT_EQ(rc, syscall::ESRCH);
}
