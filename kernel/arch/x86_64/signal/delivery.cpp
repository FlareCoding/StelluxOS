#include "signal/delivery.h"
#include "arch/arch_signal.h"
#include "defs/segments.h"
#include "sched/fpu.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

namespace x86 {

// AMD64 leaf functions may use 128 bytes below RSP, preserve it.
constexpr uint64_t RED_ZONE = 128;

// User half of the canonical address space, RIP must stay below it or
// SYSRET faults with kernel privilege on a non-canonical value.
constexpr uint64_t USER_ADDR_LIMIT = 0x0000800000000000ULL;

// RFLAGS bits a restored context may carry (arithmetic, direction, trap,
// AC, ID). IF is forced on and bit 1 is reserved-must-be-one, everything
// else (IOPL, NT, VM) is dropped so a forged frame cannot gain privilege.
constexpr uint64_t RFLAGS_USER_MASK =
    (1ULL << 0) | (1ULL << 2) | (1ULL << 4) | (1ULL << 6) | (1ULL << 7) |
    (1ULL << 8) | (1ULL << 10) | (1ULL << 11) | (1ULL << 18) | (1ULL << 21);
constexpr uint64_t RFLAGS_IF  = 1ULL << 9;
constexpr uint64_t RFLAGS_MB1 = 1ULL << 1;

static inline uint64_t align_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

__PRIVILEGED_CODE void pack_sigframe(rt_sigframe* frame,
                                     const syscall_frame* ctx,
                                     int64_t saved_result, uint32_t sig,
                                     signals::sig_set_t old_blocked,
                                     uint64_t user_fpstate) {
    sigcontext& sc = frame->uc.uc_mcontext;
    sc.r8  = ctx->r8;
    sc.r9  = ctx->r9;
    sc.r10 = ctx->r10;
    sc.r12 = ctx->r12;
    sc.r13 = ctx->r13;
    sc.r14 = ctx->r14;
    sc.r15 = ctx->r15;
    sc.rdi = ctx->rdi;
    sc.rsi = ctx->rsi;
    sc.rbp = ctx->rbp;
    sc.rbx = ctx->rbx;
    sc.rdx = ctx->rdx;
    sc.rax = static_cast<uint64_t>(saved_result);
    sc.rsp = ctx->rsp;
    sc.rip = ctx->rip;
    sc.eflags = ctx->rflags;
    sc.cs = USER_CS;
    sc.ss = USER_DS;
    sc.fpstate = user_fpstate;

    frame->uc.uc_sigmask = old_blocked;
    frame->info.si_signo = static_cast<int32_t>(sig);
    frame->info.si_code = SI_USER;
}

__PRIVILEGED_CODE bool unpack_sigframe(const rt_sigframe* frame,
                                       syscall_frame* ctx,
                                       signals::sig_set_t* mask) {
    const sigcontext& sc = frame->uc.uc_mcontext;
    if (sc.rip >= USER_ADDR_LIMIT) {
        return false;
    }

    ctx->r8  = sc.r8;
    ctx->r9  = sc.r9;
    ctx->r10 = sc.r10;
    ctx->r12 = sc.r12;
    ctx->r13 = sc.r13;
    ctx->r14 = sc.r14;
    ctx->r15 = sc.r15;
    ctx->rdi = sc.rdi;
    ctx->rsi = sc.rsi;
    ctx->rbp = sc.rbp;
    ctx->rbx = sc.rbx;
    ctx->rdx = sc.rdx;
    ctx->rsp = sc.rsp;
    ctx->rip = sc.rip;
    ctx->rflags = (sc.eflags & RFLAGS_USER_MASK) | RFLAGS_IF | RFLAGS_MB1;

    *mask = frame->uc.uc_sigmask;
    return true;
}

__PRIVILEGED_CODE int32_t build_signal_frame(syscall_frame* ctx, uint32_t sig,
                                             const signals::k_sigaction* act,
                                             signals::sig_set_t old_blocked,
                                             int64_t saved_result) {
    // SYSRET faults in Ring 0 on a non-canonical RIP, so a handler outside
    // the user half must never reach the return path
    if (act->handler >= USER_ADDR_LIMIT) {
        return -1;
    }

    // FXSAVE image above the frame, frame base at RSP % 16 == 8 so the
    // handler entry sees the ABI-required alignment after its return slot.
    uint64_t sp = ctx->rsp - RED_ZONE;
    uint64_t fpstate = align_down(sp - sizeof(sched::fpu_state), 16);
    uint64_t frame_addr = align_down(fpstate - sizeof(rt_sigframe), 16) - 8;

    rt_sigframe* frame = heap::kalloc_new<rt_sigframe>();
    if (!frame) {
        return -1;
    }

    pack_sigframe(frame, ctx, saved_result, sig, old_blocked, fpstate);
    frame->pretcode = act->restorer;

    sched::fpu_state fp;
    fpu::save(&fp);

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(frame_addr), frame, sizeof(*frame));

    if (rc == mm::uaccess::OK) {
        rc = mm::uaccess::copy_to_user(
            reinterpret_cast<void*>(fpstate), &fp, sizeof(fp));
    }

    heap::kfree_delete(frame);

    if (rc != mm::uaccess::OK) {
        return rc;
    }

    ctx->rip = act->handler;
    ctx->rsp = frame_addr;
    ctx->rdi = sig;
    ctx->rsi = frame_addr + __builtin_offsetof(rt_sigframe, info);
    ctx->rdx = frame_addr + __builtin_offsetof(rt_sigframe, uc);
    return 0;
}

__PRIVILEGED_CODE int64_t restore_signal_frame(syscall_frame* ctx) {
    // The handler's RET popped pretcode, so the frame sits one slot below.
    uint64_t frame_addr = ctx->rsp - 8;

    rt_sigframe* frame = heap::kalloc_new<rt_sigframe>();
    if (!frame) {
        signals::die_from_signal(signals::SIGSEGV);
    }

    signals::sig_set_t mask = 0;
    bool ok = mm::uaccess::copy_from_user(
        frame, reinterpret_cast<void*>(frame_addr), sizeof(*frame)) == mm::uaccess::OK;

    if (ok) {
        ok = unpack_sigframe(frame, ctx, &mask);
    }

    if (!ok) {
        heap::kfree_delete(frame);
        signals::die_from_signal(signals::SIGSEGV);
    }

    signals::set_blocked(sched::current(), signals::SIG_SETMASK, &mask, nullptr);

    uint64_t fpstate = frame->uc.uc_mcontext.fpstate;
    int64_t resume = static_cast<int64_t>(frame->uc.uc_mcontext.rax);
    if (fpstate) {
        // A bad FXSAVE pointer is a corrupt frame, kill like the other paths
        sched::fpu_state fp;
        if (mm::uaccess::copy_from_user(
                &fp, reinterpret_cast<void*>(fpstate), sizeof(fp)) != mm::uaccess::OK) {
            heap::kfree_delete(frame);
            signals::die_from_signal(signals::SIGSEGV);
        }
        fpu::sanitize_user_mxcsr(&fp);
        fpu::restore(&fp);
    }

    heap::kfree_delete(frame);
    return resume;
}

} // namespace x86

__PRIVILEGED_CODE int64_t arch::deliver_pending_signal(sched::task* self,
                                                       int64_t result) {
    uint32_t sig = 0;
    signals::k_sigaction act{};
    signals::sig_set_t old_blocked = 0;
    if (!signals::take_deliverable(self, &sig, &act, &old_blocked)) {
        return result;
    }

    // The handler returns through the restorer's rt_sigreturn,
    // an action installed without one is undeliverable.
    if (!(act.flags & signals::SA_RESTORER) || !act.restorer) {
        signals::die_from_signal(signals::SIGSEGV);
    }

    if (x86::build_signal_frame(x86::current_syscall_frame(), sig, &act,
                                old_blocked, result) != 0) {
        signals::die_from_signal(signals::SIGSEGV);
    }
    return result;
}

__PRIVILEGED_CODE int64_t arch::restore_signal_context() {
    return x86::restore_signal_frame(x86::current_syscall_frame());
}
