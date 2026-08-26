#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "signals/signal.h"
#include "signals/delivery.h"
#include "sched/fpu.h"
#include "sched/task.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"

TEST_SUITE(signal_delivery);

// Minimal process: a thread group with a leader, for signal selection.
static sched::thread_group* g_tg;
static sched::task* g_leader;

static int32_t setup_group() {
    RUN_ELEVATED({
        g_leader = heap::kalloc_new<sched::task>();
        g_tg     = heap::kalloc_new<sched::thread_group>();
    });
    if (!g_leader || !g_tg) {
        return -1;
    }

    g_tg->lock = sync::SPINLOCK_INIT;
    g_tg->leader = g_leader;
    g_tg->pid = 1;
    g_tg->threads.init();
    g_leader->group = g_tg;
    return 0;
}

static int32_t teardown_group() {
    RUN_ELEVATED({
        if (g_tg)     heap::kfree_delete(g_tg);
        if (g_leader) heap::kfree_delete(g_leader);
    });
    g_tg = nullptr;
    g_leader = nullptr;
    return 0;
}

BEFORE_EACH(signal_delivery, setup_group);
AFTER_EACH(signal_delivery, teardown_group);

static void install_handler(uint32_t sig) {
    g_tg->sig.actions[sig - 1].handler = 0x400000;
}

TEST(signal_delivery, selects_pending_handled_signal) {
    uint32_t sig = 0;
    install_handler(signals::SIGUSR1);
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGUSR1));
    RUN_ELEVATED({ sig = signals::next_deliverable(g_leader); });
    EXPECT_EQ(sig, signals::SIGUSR1);
}

TEST(signal_delivery, ignores_default_and_blocked_signals) {
    uint32_t sig = 0;

    // A pending signal left at its default action is not for a handler
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGTERM));
    RUN_ELEVATED({ sig = signals::next_deliverable(g_leader); });
    EXPECT_EQ(sig, 0U);

    // A handled signal that is blocked is not deliverable
    install_handler(signals::SIGUSR1);
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGUSR1));
    g_leader->sig.blocked.fetch_or_relaxed(signals::sig_bit(signals::SIGUSR1));
    RUN_ELEVATED({ sig = signals::next_deliverable(g_leader); });
    EXPECT_EQ(sig, 0U);
}

TEST(signal_delivery, selects_lowest_handled_signal) {
    uint32_t sig = 0;
    install_handler(signals::SIGUSR1); // 10
    install_handler(signals::SIGUSR2); // 12

    // A default-action SIGTERM pending alongside must not win over a handler
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGUSR2));
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGUSR1));
    g_leader->sig.pending.fetch_or_relaxed(signals::sig_bit(signals::SIGTERM));
    RUN_ELEVATED({ sig = signals::next_deliverable(g_leader); });
    EXPECT_EQ(sig, signals::SIGUSR1);
}

#ifdef __x86_64__
TEST(signal_delivery, x86_frame_round_trip_preserves_registers) {
    x86::syscall_frame ctx;
    ctx.rdi = 0x1000; ctx.rsi = 0x1001; ctx.rdx = 0x1002; ctx.r10 = 0x1003;
    ctx.r8 = 0x1004; ctx.r9 = 0x1005; ctx.rbx = 0x1006; ctx.rbp = 0x1007;
    ctx.r12 = 0x1008; ctx.r13 = 0x1009; ctx.r14 = 0x100a; ctx.r15 = 0x100b;
    ctx.rsp = 0x7fff0000; ctx.rip = 0x401000; ctx.rflags = 0xFFFFFFFF;

    x86::rt_sigframe* frame = nullptr;
    RUN_ELEVATED({ frame = heap::kalloc_new<x86::rt_sigframe>(); });
    ASSERT_TRUE(frame != nullptr);

    signals::sig_set_t mask = 0;
    bool ok = false;
    x86::syscall_frame out;
    RUN_ELEVATED({
        x86::pack_sigframe(frame, &ctx, 0x2222, signals::SIGINT, 0xABCD, 0x7ffe0000);
        ok = x86::unpack_sigframe(frame, &out, &mask);
    });

    EXPECT_TRUE(ok);
    EXPECT_EQ(out.rdi, ctx.rdi);
    EXPECT_EQ(out.r15, ctx.r15);
    EXPECT_EQ(out.rsp, ctx.rsp);
    EXPECT_EQ(out.rip, ctx.rip);
    EXPECT_EQ(mask, 0xABCDULL);
    EXPECT_EQ(frame->uc.uc_mcontext.rax, 0x2222ULL);

    // RFLAGS is sanitized: IF forced on, IOPL/NT cleared, user bits kept
    EXPECT_TRUE((out.rflags & (1ULL << 9)) != 0);
    EXPECT_TRUE((out.rflags & (3ULL << 12)) == 0);
    EXPECT_TRUE((out.rflags & (1ULL << 14)) == 0);
    EXPECT_TRUE((out.rflags & (1ULL << 0)) != 0);

    RUN_ELEVATED({ heap::kfree_delete(frame); });
}

TEST(signal_delivery, x86_rejects_non_canonical_return_rip) {
    x86::syscall_frame ctx;
    ctx.rdi = 0; ctx.rsi = 0; ctx.rdx = 0; ctx.r10 = 0; ctx.r8 = 0; ctx.r9 = 0;
    ctx.rbx = 0; ctx.rbp = 0; ctx.r12 = 0; ctx.r13 = 0; ctx.r14 = 0; ctx.r15 = 0;
    ctx.rsp = 0x7fff0000; ctx.rip = 0x401000; ctx.rflags = 0x2;

    x86::rt_sigframe* frame = nullptr;
    RUN_ELEVATED({ frame = heap::kalloc_new<x86::rt_sigframe>(); });
    ASSERT_TRUE(frame != nullptr);

    signals::sig_set_t mask = 0;
    bool ok = true;
    x86::syscall_frame out;
    out.rip = 0xDEAD;
    RUN_ELEVATED({
        x86::pack_sigframe(frame, &ctx, 0, signals::SIGINT, 0, 0x7ffe0000);
        frame->uc.uc_mcontext.rip = 0x0000800000000000ULL; // first kernel-half address
        ok = x86::unpack_sigframe(frame, &out, &mask);
    });

    EXPECT_TRUE(!ok);
    EXPECT_EQ(out.rip, 0xDEADULL); // ctx untouched on rejection

    RUN_ELEVATED({ heap::kfree_delete(frame); });
}

TEST(signal_delivery, x86_rejects_non_canonical_handler) {
    x86::syscall_frame ctx;
    ctx.rsp = 0x7fff0000;
    ctx.rip = 0xDEAD;

    signals::k_sigaction act = {};
    act.handler = 0x0000800000000000ULL; // first kernel-half address

    int32_t rc = 0;
    RUN_ELEVATED({ rc = x86::build_signal_frame(&ctx, signals::SIGINT, &act, 0, 0); });

    EXPECT_TRUE(rc < 0);
    EXPECT_EQ(ctx.rip, 0xDEADULL); // ctx untouched on rejection
}

TEST(signal_delivery, x86_sanitizes_restored_mxcsr) {
    sched::fpu_state fp;
    uint32_t mxcsr = 0;

    RUN_ELEVATED({
        fpu::init_state(&fp);
        *reinterpret_cast<uint32_t*>(&fp.fxsave_area[fpu::MXCSR_OFFSET]) = 0xFFFFFFFF;
        fpu::sanitize_user_mxcsr(&fp);
        mxcsr = *reinterpret_cast<uint32_t*>(&fp.fxsave_area[fpu::MXCSR_OFFSET]);
    });

    EXPECT_EQ(mxcsr >> 16, 0U); // reserved bits never reach FXRSTOR
    EXPECT_EQ(mxcsr & 0x1F80U, 0x1F80U); // supported control bits survive
}

TEST(signal_delivery, x86_full_frame_captures_scratch_registers) {
    x86::trap_frame tf{};
    tf.rax = 0x2001; tf.rcx = 0x2002; tf.r11 = 0x2003;
    tf.rdi = 0x2004; tf.rsp = 0x7fff0000; tf.rip = 0x401000;
    tf.rflags = 0x246;

    x86::rt_sigframe* frame = nullptr;
    RUN_ELEVATED({ frame = heap::kalloc_new<x86::rt_sigframe>(); });
    ASSERT_TRUE(frame != nullptr);

    RUN_ELEVATED({
        x86::pack_sigframe_full(frame, &tf, signals::SIGINT, 0xABCD, 0x7ffe0000);
    });

    // The scratch registers a syscall boundary never carries are captured
    EXPECT_EQ(frame->uc.uc_mcontext.rax, 0x2001ULL);
    EXPECT_EQ(frame->uc.uc_mcontext.rcx, 0x2002ULL);
    EXPECT_EQ(frame->uc.uc_mcontext.r11, 0x2003ULL);
    EXPECT_EQ(frame->uc.uc_flags & x86::UC_FULL_RESTORE, x86::UC_FULL_RESTORE);
    EXPECT_EQ(frame->uc.uc_sigmask, 0xABCDULL);

    RUN_ELEVATED({ heap::kfree_delete(frame); });
}

TEST(signal_delivery, x86_full_frame_round_trip) {
    x86::trap_frame tf{};
    tf.rax = 0x3001; tf.rcx = 0x3002; tf.r11 = 0x3003; tf.rbx = 0x3004;
    tf.rbp = 0x3005; tf.r15 = 0x3006; tf.rsi = 0x3007; tf.rdx = 0x3008;
    tf.rsp = 0x7fff0000; tf.rip = 0x401000; tf.rflags = 0x10246;

    x86::rt_sigframe* frame = nullptr;
    RUN_ELEVATED({ frame = heap::kalloc_new<x86::rt_sigframe>(); });
    ASSERT_TRUE(frame != nullptr);

    signals::sig_set_t mask = 0;
    bool ok = false;
    sched::thread_cpu_context out{};
    RUN_ELEVATED({
        x86::pack_sigframe_full(frame, &tf, signals::SIGUSR1, 0x77, 0);
        ok = x86::unpack_sigframe_full(frame, &out, &mask);
    });

    EXPECT_TRUE(ok);
    EXPECT_EQ(out.rax, tf.rax);
    EXPECT_EQ(out.rcx, tf.rcx);
    EXPECT_EQ(out.r11, tf.r11);
    EXPECT_EQ(out.rbx, tf.rbx);
    EXPECT_EQ(out.r15, tf.r15);
    EXPECT_EQ(out.rsp, tf.rsp);
    EXPECT_EQ(out.rip, tf.rip);
    EXPECT_EQ(mask, 0x77ULL);

    RUN_ELEVATED({ heap::kfree_delete(frame); });
}

TEST(signal_delivery, x86_full_restore_sanitizes_forged_context) {
    x86::trap_frame tf{};
    tf.rsp = 0x7fff0000; tf.rip = 0x401000; tf.rflags = 0x246;

    x86::rt_sigframe* frame = nullptr;
    RUN_ELEVATED({ frame = heap::kalloc_new<x86::rt_sigframe>(); });
    ASSERT_TRUE(frame != nullptr);

    signals::sig_set_t mask = 0;
    bool ok = false;
    sched::thread_cpu_context out{};
    uint16_t user_cs = 0;
    uint16_t user_ss = 0;
    RUN_ELEVATED({
        x86::pack_sigframe_full(frame, &tf, signals::SIGUSR1, 0, 0);
        user_cs = frame->uc.uc_mcontext.cs;
        user_ss = frame->uc.uc_mcontext.ss;

        // Forge ring 0 segments and privileged RFLAGS bits
        frame->uc.uc_mcontext.cs = 0x08;
        frame->uc.uc_mcontext.ss = 0x10;
        frame->uc.uc_mcontext.eflags = 0xFFFFFFFF;
        ok = x86::unpack_sigframe_full(frame, &out, &mask);
    });

    EXPECT_TRUE(ok);
    EXPECT_EQ(out.cs, static_cast<uint64_t>(user_cs));
    EXPECT_EQ(out.ss, static_cast<uint64_t>(user_ss));
    EXPECT_TRUE((out.rflags & (3ULL << 12)) == 0); // IOPL cleared
    EXPECT_TRUE((out.rflags & (1ULL << 9)) != 0); // IF forced on

    // A kernel-half RIP rejects the whole frame
    bool ok2 = true;
    RUN_ELEVATED({
        frame->uc.uc_mcontext.rip = 0x0000800000000000ULL;
        ok2 = x86::unpack_sigframe_full(frame, &out, &mask);
    });
    EXPECT_TRUE(!ok2);

    RUN_ELEVATED({ heap::kfree_delete(frame); });
}
#endif

#ifdef __aarch64__
TEST(signal_delivery, aarch64_frame_round_trip_preserves_state) {
    aarch64::trap_frame tf;
    for (uint32_t i = 0; i < 31; i++) {
        tf.x[i] = 0x2000 + i;
    }
    tf.sp = 0x7fff0000; tf.elr = 0x401000; tf.far = 0;
    tf.spsr = 0xF0000005; // NZCV set, EL1h mode bits that must be dropped

    sched::fpu_state fp;
    RUN_ELEVATED({ fpu::init_state(&fp); });
    fp.fpsr = 0x11; fp.fpcr = 0x22;
    fp.vregs[3][7] = 0x5A;

    aarch64::rt_sigframe* frame = nullptr;
    RUN_ELEVATED({ frame = heap::kalloc_new<aarch64::rt_sigframe>(); });
    ASSERT_TRUE(frame != nullptr);

    aarch64::trap_frame out;
    sched::fpu_state out_fp;
    signals::sig_set_t mask = 0;
    bool ok = false;
    RUN_ELEVATED({
        fpu::init_state(&out_fp);
        aarch64::pack_sigframe(frame, &tf, 0x2222, signals::SIGINT, 0xABCD, &fp);
        ok = aarch64::unpack_sigframe(frame, &out, &out_fp, &mask);
    });

    EXPECT_TRUE(ok);
    EXPECT_EQ(out.x[0], 0x2222ULL); // x0 holds the resumed syscall result
    EXPECT_EQ(out.x[30], tf.x[30]);
    EXPECT_EQ(out.sp, tf.sp);
    EXPECT_EQ(out.elr, tf.elr);
    EXPECT_EQ(mask, 0xABCDULL);
    EXPECT_EQ(out_fp.fpsr, 0x11U);
    EXPECT_EQ(out_fp.fpcr, 0x22U);
    EXPECT_EQ(out_fp.vregs[3][7], 0x5A);

    // PSTATE is forced back to EL0t with interrupts unmasked, NZCV kept
    EXPECT_EQ(out.spsr, 0xF0000000ULL);

    RUN_ELEVATED({ heap::kfree_delete(frame); });
}

TEST(signal_delivery, aarch64_rejects_corrupt_fpsimd_record) {
    aarch64::trap_frame tf;
    for (uint32_t i = 0; i < 31; i++) {
        tf.x[i] = 0x2000 + i;
    }
    tf.sp = 0x7fff0000; tf.elr = 0x401000; tf.far = 0; tf.spsr = 0xF0000000;

    sched::fpu_state fp;
    RUN_ELEVATED({ fpu::init_state(&fp); });

    aarch64::rt_sigframe* frame = nullptr;
    RUN_ELEVATED({ frame = heap::kalloc_new<aarch64::rt_sigframe>(); });
    ASSERT_TRUE(frame != nullptr);

    aarch64::trap_frame out;
    out.elr = 0xDEAD;
    sched::fpu_state out_fp;
    signals::sig_set_t mask = 0;
    bool ok = true;
    RUN_ELEVATED({
        fpu::init_state(&out_fp);
        aarch64::pack_sigframe(frame, &tf, 0, signals::SIGINT, 0, &fp);
        frame->uc.uc_mcontext.__reserved[0] ^= 0xFF; // corrupt the FPSIMD magic
        ok = aarch64::unpack_sigframe(frame, &out, &out_fp, &mask);
    });

    EXPECT_TRUE(!ok);
    EXPECT_EQ(out.elr, 0xDEADULL); // tf untouched on rejection

    RUN_ELEVATED({ heap::kfree_delete(frame); });
}
#endif
