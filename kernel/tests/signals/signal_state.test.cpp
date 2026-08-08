#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "signals/signal.h"
#include "sched/task.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(signal_state);

// Minimal process setup: a thread group with a leader and one thread.
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

BEFORE_EACH(signal_state, setup_group);
AFTER_EACH(signal_state, teardown_group);

TEST(signal_state, set_blocked_block_unblock_setmask) {
    int32_t rc = 0;
    signals::sig_set_t set = 0;
    signals::sig_set_t old = 0;

    set = signals::sig_bit(signals::SIGINT) | signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, signals::SIG_BLOCK, &set, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old, 0ULL);
    EXPECT_EQ(g_leader->sig.blocked, set);

    signals::sig_set_t unblock = signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, signals::SIG_UNBLOCK, &unblock, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old, set);
    EXPECT_EQ(g_leader->sig.blocked, signals::sig_bit(signals::SIGINT));

    signals::sig_set_t mask = signals::sig_bit(signals::SIGUSR1);
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, signals::SIG_SETMASK, &mask, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_leader->sig.blocked, mask);
}

TEST(signal_state, set_blocked_never_blocks_kill_stop) {
    int32_t rc = 0;
    signals::sig_set_t all = ~0ULL;
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, signals::SIG_SETMASK, &all, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_BITS_CLEAR(g_leader->sig.blocked, signals::UNBLOCKABLE_MASK);
    EXPECT_EQ(g_leader->sig.blocked, ~0ULL & ~signals::UNBLOCKABLE_MASK);
}

TEST(signal_state, set_blocked_query_only_ignores_how) {
    int32_t rc = 0;
    signals::sig_set_t old = ~0ULL;
    g_leader->sig.blocked = signals::sig_bit(signals::SIGINT);

    // set == nullptr: how is not significant (POSIX)
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, 99, nullptr, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old, signals::sig_bit(signals::SIGINT));
    EXPECT_EQ(g_leader->sig.blocked, signals::sig_bit(signals::SIGINT));
}

TEST(signal_state, set_blocked_rejects_bad_how) {
    int32_t rc = 0;
    signals::sig_set_t set = signals::sig_bit(signals::SIGINT);
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, 99, &set, nullptr);
    });
    EXPECT_EQ(rc, signals::ERR_INVAL);
    EXPECT_EQ(g_leader->sig.blocked, 0ULL);
}

TEST(signal_state, set_action_roundtrip) {
    int32_t rc = 0;
    signals::k_sigaction act = {};
    act.handler  = 0x400000;
    act.flags    = signals::SA_RESTORER | signals::SA_RESTART;
    act.restorer = 0x500000;
    act.mask     = signals::sig_bit(signals::SIGUSR1);

    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGINT, &act, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);

    signals::k_sigaction old = {};
    signals::k_sigaction repl = {};
    repl.handler = signals::SIG_DFL;
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGINT, &repl, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old.handler, 0x400000ULL);
    EXPECT_EQ(old.flags, signals::SA_RESTORER | signals::SA_RESTART);
    EXPECT_EQ(old.restorer, 0x500000ULL);
    EXPECT_EQ(old.mask, signals::sig_bit(signals::SIGUSR1));
}

TEST(signal_state, set_action_rejects_invalid) {
    int32_t rc = 0;
    signals::k_sigaction act = {};

    RUN_ELEVATED({ rc = signals::set_action(g_tg, 0, &act, nullptr); });
    EXPECT_EQ(rc, signals::ERR_INVAL);

    RUN_ELEVATED({ rc = signals::set_action(g_tg, 65, &act, nullptr); });
    EXPECT_EQ(rc, signals::ERR_INVAL);

    RUN_ELEVATED({ rc = signals::set_action(g_tg, signals::SIGKILL, &act, nullptr); });
    EXPECT_EQ(rc, signals::ERR_INVAL);

    RUN_ELEVATED({ rc = signals::set_action(g_tg, signals::SIGSTOP, &act, nullptr); });
    EXPECT_EQ(rc, signals::ERR_INVAL);

    RUN_ELEVATED({ rc = signals::set_action(nullptr, signals::SIGINT, &act, nullptr); });
    EXPECT_EQ(rc, signals::ERR_INVAL);
}

TEST(signal_state, set_action_allows_query_of_kill) {
    int32_t rc = 0;
    signals::k_sigaction old = {};
    old.handler = 0xDEAD;
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGKILL, nullptr, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old.handler, signals::SIG_DFL);
}

TEST(signal_state, dfl_action_classification) {
    EXPECT_TRUE(signals::dfl_action(signals::SIGTERM) == signals::default_action::TERM);
    EXPECT_TRUE(signals::dfl_action(signals::SIGKILL) == signals::default_action::TERM);
    EXPECT_TRUE(signals::dfl_action(signals::SIGSEGV) == signals::default_action::TERM);
    EXPECT_TRUE(signals::dfl_action(signals::SIGCHLD) == signals::default_action::IGNORE);
    EXPECT_TRUE(signals::dfl_action(signals::SIGCONT) == signals::default_action::IGNORE);
    EXPECT_TRUE(signals::dfl_action(signals::SIGURG) == signals::default_action::IGNORE);
    EXPECT_TRUE(signals::dfl_action(signals::SIGWINCH) == signals::default_action::IGNORE);
    EXPECT_TRUE(signals::dfl_action(signals::SIGSTOP) == signals::default_action::STOP);
    EXPECT_TRUE(signals::dfl_action(signals::SIGTSTP) == signals::default_action::STOP);
    EXPECT_TRUE(signals::dfl_action(64) == signals::default_action::TERM);
}

TEST(signal_state, set_action_strips_unblockables_from_mask) {
    signals::k_sigaction act = {};
    act.handler = 0x400000;
    act.mask = signals::sig_bit(signals::SIGUSR1) | signals::UNBLOCKABLE_MASK;

    int32_t rc = 0;
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGINT, &act, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);

    signals::k_sigaction old = {};
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGINT, nullptr, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old.mask, signals::sig_bit(signals::SIGUSR1));
}

TEST(signal_state, dfl_on_default_ignore_signal_discards_pending) {
    const signals::sig_set_t sigchld = signals::sig_bit(signals::SIGCHLD);
    const signals::sig_set_t sigtstp = signals::sig_bit(signals::SIGTSTP);

    g_tg->sig.shared_pending = sigchld | sigtstp;
    g_leader->sig.pending    = sigchld | sigtstp;

    signals::k_sigaction act = {};
    act.handler = signals::SIG_DFL;
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGCHLD, &act, nullptr);
        if (rc == signals::OK) {
            rc = signals::set_action(g_tg, signals::SIGTSTP, &act, nullptr);
        }
    });
    EXPECT_EQ(rc, signals::OK);

    // Default-ignore SIGCHLD is discarded, stop-class SIGTSTP is not
    EXPECT_EQ(g_tg->sig.shared_pending, sigtstp);
    EXPECT_EQ(g_leader->sig.pending, sigtstp);
}

TEST(signal_state, sig_ign_discards_pending) {
    const signals::sig_set_t sigusr1 = signals::sig_bit(signals::SIGUSR1);
    const signals::sig_set_t sigterm = signals::sig_bit(signals::SIGTERM);

    g_tg->sig.shared_pending = sigusr1 | sigterm;
    g_leader->sig.pending    = sigusr1 | sigterm;
    g_thread->sig.pending    = sigusr1 | sigterm;

    signals::k_sigaction act = {};
    act.handler = signals::SIG_IGN;
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGUSR1, &act, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);

    // SIGUSR1 discarded everywhere, SIGTERM untouched
    EXPECT_EQ(g_tg->sig.shared_pending, sigterm);
    EXPECT_EQ(g_leader->sig.pending, sigterm);
    EXPECT_EQ(g_thread->sig.pending, sigterm);
}

TEST(signal_state, pending_blocked_set_combines_sets) {
    const signals::sig_set_t sigint  = signals::sig_bit(signals::SIGINT);
    const signals::sig_set_t sigusr1 = signals::sig_bit(signals::SIGUSR1);
    const signals::sig_set_t sigterm = signals::sig_bit(signals::SIGTERM);

    g_leader->sig.pending     = sigint;
    g_tg->sig.shared_pending  = sigusr1 | sigterm;
    g_leader->sig.blocked     = sigint | sigusr1;

    signals::sig_set_t result = 0;
    RUN_ELEVATED({
        result = signals::pending_blocked_set(g_leader);
    });

    // Only pending signals that are blocked are reported (rt_sigpending)
    EXPECT_EQ(result, sigint | sigusr1);
}
