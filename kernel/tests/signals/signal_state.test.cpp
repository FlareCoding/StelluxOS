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
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), set);

    signals::sig_set_t unblock = signals::sig_bit(signals::SIGTERM);
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, signals::SIG_UNBLOCK, &unblock, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old, set);
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), signals::sig_bit(signals::SIGINT));

    signals::sig_set_t mask = signals::sig_bit(signals::SIGUSR1);
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, signals::SIG_SETMASK, &mask, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), mask);
}

TEST(signal_state, set_blocked_never_blocks_kill_stop) {
    int32_t rc = 0;
    signals::sig_set_t all = ~0ULL;
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, signals::SIG_SETMASK, &all, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_BITS_CLEAR(g_leader->sig.blocked.load_relaxed(), signals::UNBLOCKABLE_MASK);
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), ~0ULL & ~signals::UNBLOCKABLE_MASK);
}

TEST(signal_state, set_blocked_query_only_ignores_how) {
    int32_t rc = 0;
    signals::sig_set_t old = ~0ULL;
    g_leader->sig.blocked .store_relaxed(signals::sig_bit(signals::SIGINT));

    // set == nullptr: how is not significant (POSIX)
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, 99, nullptr, &old);
    });
    EXPECT_EQ(rc, signals::OK);
    EXPECT_EQ(old, signals::sig_bit(signals::SIGINT));
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), signals::sig_bit(signals::SIGINT));
}

TEST(signal_state, set_blocked_rejects_bad_how) {
    int32_t rc = 0;
    signals::sig_set_t set = signals::sig_bit(signals::SIGINT);
    RUN_ELEVATED({
        rc = signals::set_blocked(g_leader, 99, &set, nullptr);
    });
    EXPECT_EQ(rc, signals::ERR_INVAL);
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), 0ULL);
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

    g_tg->sig.shared_pending .store_relaxed(sigchld | sigtstp);
    g_leader->sig.pending    .store_relaxed(sigchld | sigtstp);

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
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), sigtstp);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), sigtstp);
}

TEST(signal_state, sig_ign_discards_pending) {
    const signals::sig_set_t sigusr1 = signals::sig_bit(signals::SIGUSR1);
    const signals::sig_set_t sigterm = signals::sig_bit(signals::SIGTERM);

    g_tg->sig.shared_pending .store_relaxed(sigusr1 | sigterm);
    g_leader->sig.pending    .store_relaxed(sigusr1 | sigterm);
    g_thread->sig.pending    .store_relaxed(sigusr1 | sigterm);

    signals::k_sigaction act = {};
    act.handler = signals::SIG_IGN;
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = signals::set_action(g_tg, signals::SIGUSR1, &act, nullptr);
    });
    EXPECT_EQ(rc, signals::OK);

    // SIGUSR1 discarded everywhere, SIGTERM untouched
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), sigterm);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), sigterm);
    EXPECT_EQ(g_thread->sig.pending.load_relaxed(), sigterm);
}

TEST(signal_state, pending_blocked_set_combines_sets) {
    const signals::sig_set_t sigint  = signals::sig_bit(signals::SIGINT);
    const signals::sig_set_t sigusr1 = signals::sig_bit(signals::SIGUSR1);
    const signals::sig_set_t sigterm = signals::sig_bit(signals::SIGTERM);

    g_leader->sig.pending     .store_relaxed(sigint);
    g_tg->sig.shared_pending  .store_relaxed(sigusr1 | sigterm);
    g_leader->sig.blocked     .store_relaxed(sigint | sigusr1);

    signals::sig_set_t result = 0;
    RUN_ELEVATED({
        result = signals::pending_blocked_set(g_leader);
    });

    // Only pending signals that are blocked are reported (rt_sigpending)
    EXPECT_EQ(result, sigint | sigusr1);
}

// Installs a handler for sig with the given flags and sa_mask
static void install_handler(uint32_t sig, uint64_t flags,
                            signals::sig_set_t mask) {
    signals::k_sigaction act = {};
    act.handler = 0x400000;
    act.flags = flags;
    act.restorer = 0x400100;
    act.mask = mask;
    RUN_ELEVATED({ signals::set_action(g_tg, sig, &act, nullptr); });
}

TEST(signal_state, take_deliverable_consumes_and_masks) {
    const signals::sig_set_t handler_mask = signals::sig_bit(signals::SIGUSR2);
    install_handler(signals::SIGUSR1, 0, handler_mask);
    g_leader->sig.pending .store_relaxed(signals::sig_bit(signals::SIGUSR1));
    g_leader->sig.blocked .store_relaxed(signals::sig_bit(signals::SIGTERM));

    uint32_t sig = 0;
    signals::k_sigaction act = {};
    signals::sig_set_t old_blocked = 0;
    bool taken = false;
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });

    EXPECT_TRUE(taken);
    EXPECT_EQ(sig, signals::SIGUSR1);
    EXPECT_EQ(act.handler, 0x400000UL);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), 0ULL);
    EXPECT_EQ(old_blocked, signals::sig_bit(signals::SIGTERM));

    // The handler runs with sa_mask plus its own signal on top of old_blocked
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), signals::sig_bit(signals::SIGTERM)
                                   | handler_mask
                                   | signals::sig_bit(signals::SIGUSR1));
}

TEST(signal_state, take_deliverable_prefers_thread_set) {
    install_handler(signals::SIGUSR1, 0, 0);
    g_leader->sig.pending    .store_relaxed(signals::sig_bit(signals::SIGUSR1));
    g_tg->sig.shared_pending .store_relaxed(signals::sig_bit(signals::SIGUSR1));

    uint32_t sig = 0;
    signals::k_sigaction act = {};
    signals::sig_set_t old_blocked = 0;
    bool taken = false;
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });

    EXPECT_TRUE(taken);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), 0ULL);
    EXPECT_EQ(g_tg->sig.shared_pending.load_relaxed(), signals::sig_bit(signals::SIGUSR1));
}

TEST(signal_state, take_deliverable_lowest_handled_first) {
    install_handler(signals::SIGUSR1, 0, 0);
    install_handler(signals::SIGUSR2, 0, 0);
    g_leader->sig.pending .store_relaxed(signals::sig_bit(signals::SIGUSR1)
                          | signals::sig_bit(signals::SIGUSR2));
    g_leader->sig.blocked .store_relaxed(0);

    uint32_t sig = 0;
    signals::k_sigaction act = {};
    signals::sig_set_t old_blocked = 0;
    bool taken = false;
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });
    EXPECT_TRUE(taken);
    EXPECT_EQ(sig, signals::SIGUSR1);

    // Reset the mask the first take installed, then the next one follows
    g_leader->sig.blocked .store_relaxed(0);
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });
    EXPECT_TRUE(taken);
    EXPECT_EQ(sig, signals::SIGUSR2);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), 0ULL);
}

TEST(signal_state, take_deliverable_nodefer_leaves_signal_unblocked) {
    install_handler(signals::SIGUSR1, signals::SA_NODEFER, 0);
    g_leader->sig.pending .store_relaxed(signals::sig_bit(signals::SIGUSR1));

    uint32_t sig = 0;
    signals::k_sigaction act = {};
    signals::sig_set_t old_blocked = 0;
    bool taken = false;
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });

    EXPECT_TRUE(taken);
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), 0ULL);
}

TEST(signal_state, take_deliverable_resethand_restores_default) {
    install_handler(signals::SIGUSR1, signals::SA_RESETHAND, 0);
    g_leader->sig.pending .store_relaxed(signals::sig_bit(signals::SIGUSR1));

    uint32_t sig = 0;
    signals::k_sigaction act = {};
    signals::sig_set_t old_blocked = 0;
    bool taken = false;
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });

    // The snapshot keeps the handler while the stored action resets
    EXPECT_TRUE(taken);
    EXPECT_EQ(act.handler, 0x400000UL);
    EXPECT_EQ(g_tg->sig.actions[signals::SIGUSR1 - 1].handler, signals::SIG_DFL);
}

TEST(signal_state, untake_deliverable_restores_state) {
    install_handler(signals::SIGUSR1, signals::SA_RESETHAND,
                    signals::sig_bit(signals::SIGUSR2));
    g_leader->sig.pending .store_relaxed(signals::sig_bit(signals::SIGUSR1));

    uint32_t sig = 0;
    signals::k_sigaction act = {};
    signals::sig_set_t old_blocked = 0;
    bool taken = false;
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });
    ASSERT_TRUE(taken);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), 0ULL);
    EXPECT_EQ(g_tg->sig.actions[signals::SIGUSR1 - 1].handler, signals::SIG_DFL);

    // A deferred delivery puts the signal, the mask, and the one-shot
    // action all back
    RUN_ELEVATED({
        signals::untake_deliverable(g_leader, sig, &act, old_blocked);
    });
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGUSR1));
    EXPECT_EQ(g_leader->sig.blocked.load_relaxed(), 0ULL);
    EXPECT_EQ(g_tg->sig.actions[signals::SIGUSR1 - 1].handler, 0x400000UL);
}

TEST(signal_state, take_deliverable_skips_blocked_and_unhandled) {
    uint32_t sig = 0;
    signals::k_sigaction act = {};
    signals::sig_set_t old_blocked = 0;
    bool taken = true;

    // Nothing pending
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });
    EXPECT_FALSE(taken);

    // Pending without a handler stays for the fatal path
    g_leader->sig.pending .store_relaxed(signals::sig_bit(signals::SIGTERM));
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });
    EXPECT_FALSE(taken);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGTERM));

    // A blocked handled signal is not deliverable
    install_handler(signals::SIGUSR1, 0, 0);
    g_leader->sig.pending .store_relaxed(signals::sig_bit(signals::SIGUSR1));
    g_leader->sig.blocked .store_relaxed(signals::sig_bit(signals::SIGUSR1));
    RUN_ELEVATED({
        taken = signals::take_deliverable(g_leader, &sig, &act, &old_blocked);
    });
    EXPECT_FALSE(taken);
    EXPECT_EQ(g_leader->sig.pending.load_relaxed(), signals::sig_bit(signals::SIGUSR1));
}
