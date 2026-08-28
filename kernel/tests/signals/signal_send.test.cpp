#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "signals/signal.h"
#include "sched/task.h"
#include "sched/task_registry.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(signal_send);

// Minimal process fixture: a thread group with a leader and one thread.
static sched::thread_group* g_tg;
static sched::task* g_leader;
static sched::task* g_thread;

static int32_t setup_group() {
    RUN_ELEVATED({
        g_leader = heap::kalloc_new<sched::task>();
        g_thread = heap::kalloc_new<sched::task>();
        g_tg     = heap::kalloc_new<sched::thread_group>();
    });
    if (!g_leader || !g_thread || !g_tg) {
        return -1;
    }

    g_tg->lock = sync::SPINLOCK_INIT;
    g_tg->leader = g_leader;
    g_tg->pid = 1;
    g_tg->threads.init();
    g_tg->thread_count = 1;

    g_leader->group = g_tg;
    g_thread->group = g_tg;
    g_tg->threads.push_back(g_thread);

    return 0;
}

static int32_t teardown_group() {
    RUN_ELEVATED({
        if (g_tg)     heap::kfree_delete(g_tg);
        if (g_leader) heap::kfree_delete(g_leader);
        if (g_thread) heap::kfree_delete(g_thread);
    });
    g_tg = nullptr;
    g_leader = nullptr;
    g_thread = nullptr;
    return 0;
}

BEFORE_EACH(signal_send, setup_group);
AFTER_EACH(signal_send, teardown_group);

TEST(signal_send, rejects_invalid_signals) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, 0); });
    EXPECT_EQ(rc, signals::ERR_INVAL);
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, 65); });
    EXPECT_EQ(rc, signals::ERR_INVAL);
    RUN_ELEVATED({ rc = signals::send_to_task(nullptr, signals::SIGTERM); });
    EXPECT_EQ(rc, signals::ERR_INVAL);
    RUN_ELEVATED({ rc = signals::send_to_group(nullptr, signals::SIGTERM); });
    EXPECT_EQ(rc, signals::ERR_INVAL);
}

TEST(signal_send, rejects_kernel_and_idle_targets) {
    int32_t rc = 0;
    g_thread->exec.flags = sched::TASK_FLAG_KERNEL;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, signals::SIGTERM); });
    EXPECT_EQ(rc, signals::ERR_PERM);

    g_thread->exec.flags = sched::TASK_FLAG_IDLE;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, signals::SIGTERM); });
    EXPECT_EQ(rc, signals::ERR_PERM);
    g_thread->exec.flags = 0;
}

TEST(signal_send, fatal_send_pends_without_kill_flag) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, signals::SIGTERM); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_thread->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGTERM));
    EXPECT_BITS_CLEAR(g_thread->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
    EXPECT_BITS_CLEAR(g_leader->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
}

TEST(signal_send, ignored_unblocked_send_drops) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, signals::SIGCHLD); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_thread->sig.pending.load_relaxed(), 0ULL);
}

TEST(signal_send, ignored_blocked_send_pends) {
    g_thread->sig.blocked .store_relaxed(signals::sig_bit(signals::SIGCHLD));
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, signals::SIGCHLD); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_thread->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGCHLD));
}

TEST(signal_send, handled_send_pends) {
    signals::k_sigaction act = {};
    act.handler = 0x400000;
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGTERM, &act, nullptr);
        if (rc == signals::OK) {
            rc = signals::send_to_task(g_thread, signals::SIGTERM);
        }
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_thread->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGTERM));
    EXPECT_BITS_CLEAR(g_thread->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
}

TEST(signal_send, sigkill_to_thread_kills_group) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, signals::SIGKILL); });
    EXPECT_EQ(rc, signals::OK);
    // SIGKILL is process-wide: target and leader are marked, and the
    // shared bit makes it fatal for every other thread immediately
    EXPECT_BITS_SET(g_thread->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
    EXPECT_BITS_SET(g_leader->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
}

TEST(signal_send, sigkill_to_group_marks_leader) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_group(g_tg, signals::SIGKILL); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_BITS_SET(g_leader->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
}

TEST(signal_send, group_fatal_send_sets_shared) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_group(g_tg, signals::SIGTERM); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), signals::sig_bit(signals::SIGTERM));
    EXPECT_BITS_CLEAR(g_leader->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGKILL));
}

TEST(signal_send, group_ignored_send_drops_unless_blocked) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_group(g_tg, signals::SIGCHLD); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), 0ULL);

    // One thread blocking the signal keeps it pending
    g_thread->sig.blocked .store_relaxed(signals::sig_bit(signals::SIGCHLD));
    RUN_ELEVATED({ rc = signals::send_to_group(g_tg, signals::SIGCHLD); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), signals::sig_bit(signals::SIGCHLD));
}

TEST(signal_send, stop_class_send_is_ignored) {
    int32_t rc = 0;
    RUN_ELEVATED({ rc = signals::send_to_task(g_thread, signals::SIGTSTP); });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_thread->sig.pending.load_relaxed(), 0ULL);
}

// A group-id send must reach every matching group, not only the ones
// that fit a fixed window. Sized past the 64-group batch an earlier
// implementation silently dropped.
constexpr uint32_t GID_GROUP_COUNT = 67;
constexpr uint32_t GID_TEST_GROUP  = 0x7E577E57u;
constexpr uint32_t GID_TID_BASE    = 0x40000000u;

static sched::task* g_gid_leaders[GID_GROUP_COUNT];
static sched::thread_group* g_gid_groups[GID_GROUP_COUNT];

TEST(signal_send, group_id_send_reaches_all_groups) {
    bool built = true;

    // Single-task mock processes registered in the live task registry,
    // all members of the same process group
    RUN_ELEVATED({
        for (uint32_t i = 0; i < GID_GROUP_COUNT; i++) {
            g_gid_leaders[i] = heap::kalloc_new<sched::task>();
            g_gid_groups[i]  = heap::kalloc_new<sched::thread_group>();
            if (!g_gid_leaders[i] || !g_gid_groups[i]) {
                built = false;
                break;
            }

            sched::task* t = g_gid_leaders[i];
            sched::thread_group* tg = g_gid_groups[i];
            tg->lock = sync::SPINLOCK_INIT;
            tg->leader = t;
            tg->pid = GID_TID_BASE + i;
            tg->threads.init();
            tg->group_id.store_relaxed(GID_TEST_GROUP);

            t->tid = GID_TID_BASE + i;
            t->group = tg;
            sched::g_task_registry.insert(t);
        }
    });

    int32_t rc = -1;
    if (built) {
        RUN_ELEVATED({
            rc = signals::send_to_group_id(GID_TEST_GROUP, signals::SIGTERM);
        });
    }

    EXPECT_TRUE(built);
    EXPECT_EQ(rc, signals::OK);

    uint32_t reached = 0;
    for (uint32_t i = 0; i < GID_GROUP_COUNT; i++) {
        if (g_gid_groups[i] && (g_gid_groups[i]->sig.shared_pending.load_relaxed()
                                & signals::sig_bit(signals::SIGTERM))) {
            reached++;
        }
    }
    EXPECT_EQ(reached, GID_GROUP_COUNT);

    RUN_ELEVATED({
        for (uint32_t i = 0; i < GID_GROUP_COUNT; i++) {
            if (g_gid_leaders[i] && g_gid_groups[i]) {
                sched::g_task_registry.remove(*g_gid_leaders[i]);
            }
            if (g_gid_groups[i])  heap::kfree_delete(g_gid_groups[i]);
            if (g_gid_leaders[i]) heap::kfree_delete(g_gid_leaders[i]);
            g_gid_groups[i]  = nullptr;
            g_gid_leaders[i] = nullptr;
        }
    });
}
