#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "signals/signal.h"
#include "sched/task.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(signal_death);

// Hand-built process: a thread group with a leader and one thread.
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

BEFORE_EACH(signal_death, setup_group);
AFTER_EACH(signal_death, teardown_group);

TEST(signal_death, killed_member_reports_group_exit_signal) {
    uint32_t fatal = 0;

    // A killed member of a group dying from SIGSEGV reports SIGSEGV
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGKILL));
    g_tg->sig.exit_signal.store_relaxed(signals::SIGSEGV);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, signals::SIGSEGV);

    // Every thread of the group reports the same signal
    g_thread->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_thread); });
    EXPECT_EQ(fatal, signals::SIGSEGV);
}

TEST(signal_death, kill_without_exit_signal_reports_sigkill) {
    uint32_t fatal = 0;
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGKILL));
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, signals::SIGKILL);
}

TEST(signal_death, exit_signal_alone_reports_nothing) {
    uint32_t fatal = 0;
    g_tg->sig.exit_signal.store_relaxed(signals::SIGTERM);

    // Without the kill flag the exit signal alone reports nothing
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, 0U);
}
