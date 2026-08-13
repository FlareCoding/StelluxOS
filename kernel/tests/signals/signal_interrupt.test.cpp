#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "signals/signal.h"
#include "sched/task.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(signal_interrupt);

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

BEFORE_EACH(signal_interrupt, setup_group);
AFTER_EACH(signal_interrupt, teardown_group);

TEST(signal_interrupt, pending_sigkill_reports_sigkill) {
    uint32_t fatal = 0;
    // SIGKILL wins over a lower pending signal
    g_leader->sig.pending = signals::sig_bit(signals::SIGKILL)
                          | signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, signals::SIGKILL);
}

TEST(signal_interrupt, lowest_fatal_signal_wins) {
    uint32_t fatal = 0;
    g_leader->sig.pending = signals::sig_bit(signals::SIGTERM)
                          | signals::sig_bit(signals::SIGINT);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, signals::SIGINT);
}

TEST(signal_interrupt, pending_sigkill_outranks_lower_signals) {
    uint32_t fatal = 0;
    g_leader->sig.pending = signals::sig_bit(signals::SIGINT);
    g_tg->sig.shared_pending = signals::sig_bit(signals::SIGKILL);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, signals::SIGKILL);
}

TEST(signal_interrupt, blocked_signals_are_not_fatal) {
    uint32_t fatal = 1;
    g_leader->sig.pending = signals::sig_bit(signals::SIGTERM);
    g_leader->sig.blocked = signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, 0U);
}

TEST(signal_interrupt, handled_signals_are_not_fatal) {
    signals::k_sigaction act = {};
    act.handler = 0x400000;
    uint32_t fatal = 1;
    RUN_ELEVATED({
        signals::set_action(g_tg, signals::SIGTERM, &act, nullptr);
    });
    g_leader->sig.pending = signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, 0U);
}

TEST(signal_interrupt, default_ignore_pending_is_not_fatal) {
    uint32_t fatal = 1;
    g_leader->sig.pending = signals::sig_bit(signals::SIGCHLD);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_leader); });
    EXPECT_EQ(fatal, 0U);
}

TEST(signal_interrupt, shared_pending_is_visible_to_every_thread) {
    uint32_t fatal = 0;
    g_tg->sig.shared_pending = signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({ fatal = signals::fatal_pending(g_thread); });
    EXPECT_EQ(fatal, signals::SIGTERM);
}

TEST(signal_interrupt, interrupt_pending_truth) {
    bool intr = true;
    RUN_ELEVATED({ intr = signals::interrupt_pending(g_leader); });
    EXPECT_FALSE(intr);

    g_leader->sig.pending = signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({ intr = signals::interrupt_pending(g_leader); });
    EXPECT_TRUE(intr);

    intr = true;
    RUN_ELEVATED({ intr = signals::interrupt_pending(nullptr); });
    EXPECT_FALSE(intr);
}

TEST(signal_interrupt, interrupt_pending_covers_handled_signals) {
    signals::k_sigaction act = {};
    act.handler = 0x400000;
    RUN_ELEVATED({
        signals::set_action(g_tg, signals::SIGUSR1, &act, nullptr);
    });

    bool intr = false;
    g_leader->sig.pending = signals::sig_bit(signals::SIGUSR1);
    RUN_ELEVATED({ intr = signals::interrupt_pending(g_leader); });
    EXPECT_TRUE(intr);

    // Blocking the signal removes the interrupt reason
    g_leader->sig.blocked = signals::sig_bit(signals::SIGUSR1);
    RUN_ELEVATED({ intr = signals::interrupt_pending(g_leader); });
    EXPECT_FALSE(intr);
}
