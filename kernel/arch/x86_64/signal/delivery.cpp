#include "signal/delivery.h"
#include "arch/arch_signal.h"
#include "syscall/syscall_table.h"
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

// SYSCALL is two bytes, stepping RIP back re-executes the interrupted call
constexpr uint64_t SYSCALL_INSN_LEN = 2;

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

    // A context captured outside a syscall restores every register through
    // the IRET exit, the frame-based SYSRET path cannot rebuild it
    bool full = ok && (frame->uc.uc_flags & UC_FULL_RESTORE) != 0;
    sched::task_exec_core* exec = this_cpu(current_task_exec);

    if (ok) {
        ok = full ? unpack_sigframe_full(frame, &exec->iret_ctx, &mask)
                  : unpack_sigframe(frame, ctx, &mask);
    }

    if (!ok) {
        heap::kfree_delete(frame);
        signals::die_from_signal(signals::SIGSEGV);
    }

    signals::set_blocked(sched::current(), signals::SIG_SETMASK, &mask, nullptr);

    uint64_t fpstate = frame->uc.uc_mcontext.fpstate;
    int64_t resume = full ? 0
        : static_cast<int64_t>(frame->uc.uc_mcontext.rax);
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

    if (full) {
        // Publish only after the context is fully staged, the syscall
        // exit consumes it as soon as this handler returns
        __atomic_store_n(&exec->iret_pending, 1u, __ATOMIC_RELEASE);
    }

    heap::kfree_delete(frame);
    return resume;
}

__PRIVILEGED_CODE void pack_sigframe_full(rt_sigframe* frame,
                                          const trap_frame* tf, uint32_t sig,
                                          signals::sig_set_t old_blocked,
                                          uint64_t user_fpstate) {
    sigcontext& sc = frame->uc.uc_mcontext;
    sc.r8  = tf->r8;
    sc.r9  = tf->r9;
    sc.r10 = tf->r10;
    sc.r11 = tf->r11;
    sc.r12 = tf->r12;
    sc.r13 = tf->r13;
    sc.r14 = tf->r14;
    sc.r15 = tf->r15;
    sc.rdi = tf->rdi;
    sc.rsi = tf->rsi;
    sc.rbp = tf->rbp;
    sc.rbx = tf->rbx;
    sc.rdx = tf->rdx;
    sc.rax = tf->rax;
    sc.rcx = tf->rcx;
    sc.rsp = tf->rsp;
    sc.rip = tf->rip;
    sc.eflags = tf->rflags;
    sc.cs = USER_CS;
    sc.ss = USER_DS;
    sc.fpstate = user_fpstate;

    frame->uc.uc_flags = UC_FULL_RESTORE;
    frame->uc.uc_sigmask = old_blocked;
    frame->info.si_signo = static_cast<int32_t>(sig);
    frame->info.si_code = SI_USER;
}

__PRIVILEGED_CODE bool unpack_sigframe_full(const rt_sigframe* frame,
                                            sched::thread_cpu_context* out,
                                            signals::sig_set_t* mask) {
    const sigcontext& sc = frame->uc.uc_mcontext;
    if (sc.rip >= USER_ADDR_LIMIT) {
        return false;
    }

    out->rax = sc.rax;
    out->rbx = sc.rbx;
    out->rcx = sc.rcx;
    out->rdx = sc.rdx;
    out->rsi = sc.rsi;
    out->rdi = sc.rdi;
    out->rbp = sc.rbp;
    out->rsp = sc.rsp;
    out->r8  = sc.r8;
    out->r9  = sc.r9;
    out->r10 = sc.r10;
    out->r11 = sc.r11;
    out->r12 = sc.r12;
    out->r13 = sc.r13;
    out->r14 = sc.r14;
    out->r15 = sc.r15;
    out->rip = sc.rip;
    out->rflags = (sc.eflags & RFLAGS_USER_MASK) | RFLAGS_IF | RFLAGS_MB1;

    // Forged segments must never reach the IRET exit
    out->cs = USER_CS;
    out->ss = USER_DS;

    *mask = frame->uc.uc_sigmask;
    return true;
}

// Direction and trap flags a handler must not inherit from the
// interrupted instruction stream
constexpr uint64_t RFLAGS_DF = 1ULL << 10;
constexpr uint64_t RFLAGS_TF = 1ULL << 8;

/**
 * Frame write for delivery outside a syscall. Never blocks, the caller
 * defers the signal when the address-space lock is contended.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t build_signal_frame_async(
    trap_frame* tf, uint32_t sig, const signals::k_sigaction* act,
    signals::sig_set_t old_blocked) {
    // SYSRET is not involved here, but the same user-half bound keeps a
    // kernel-half handler from ever running with user state
    if (act->handler >= USER_ADDR_LIMIT) {
        return -1;
    }

    uint64_t sp = tf->rsp - RED_ZONE;
    uint64_t fpstate = align_down(sp - sizeof(sched::fpu_state), 16);
    uint64_t frame_addr = align_down(fpstate - sizeof(rt_sigframe), 16) - 8;

    rt_sigframe* frame = heap::kalloc_new<rt_sigframe>();
    if (!frame) {
        return mm::uaccess::ERR_RETRY;
    }

    pack_sigframe_full(frame, tf, sig, old_blocked, fpstate);
    frame->pretcode = act->restorer;

    sched::fpu_state fp;
    fpu::save(&fp);

    int32_t rc = mm::uaccess::copy_to_user_nonblock(
        reinterpret_cast<void*>(frame_addr), frame, sizeof(*frame));
    if (rc == mm::uaccess::OK) {
        rc = mm::uaccess::copy_to_user_nonblock(
            reinterpret_cast<void*>(fpstate), &fp, sizeof(fp));
    }
    heap::kfree_delete(frame);

    if (rc != mm::uaccess::OK) {
        return rc;
    }

    tf->rip = act->handler;
    tf->rsp = frame_addr;
    tf->rdi = sig;
    tf->rsi = frame_addr + __builtin_offsetof(rt_sigframe, info);
    tf->rdx = frame_addr + __builtin_offsetof(rt_sigframe, uc);
    tf->rflags = (tf->rflags & RFLAGS_USER_MASK & ~(RFLAGS_DF | RFLAGS_TF))
        | RFLAGS_IF | RFLAGS_MB1;
    return 0;
}

__PRIVILEGED_CODE void deliver_async_signal(sched::task* self,
                                            trap_frame* tf) {
    if (!from_user(tf)) {
        return;
    }

    // An elevated task keeps its signals pending until it lowers, the
    // same rule every other delivery site applies
    if (!self || !self->group ||
        (self->exec.flags & sched::TASK_FLAG_ELEVATED) ||
        self->state == sched::TASK_STATE_DEAD) {
        return;
    }

    // A fatal signal takes the death path at this same boundary
    if (signals::fatal_pending(self)) {
        return;
    }

    uint32_t sig = 0;
    signals::k_sigaction act{};
    signals::sig_set_t old_blocked = 0;
    if (!signals::take_deliverable(self, &sig, &act, &old_blocked)) {
        return;
    }

    // The handler returns through the restorer's rt_sigreturn,
    // an action installed without one is undeliverable.
    if (!(act.flags & signals::SA_RESTORER) || !act.restorer) {
        signals::die_from_signal(signals::SIGSEGV);
    }

    int32_t rc = build_signal_frame_async(tf, sig, &act, old_blocked);
    if (rc == mm::uaccess::ERR_RETRY) {
        // Not deliverable right now, a later boundary picks it up
        signals::untake_deliverable(self, sig, &act, old_blocked);
        return;
    }
    if (rc != 0) {
        signals::die_from_signal(signals::SIGSEGV);
    }
}

} // namespace x86

__PRIVILEGED_CODE int64_t arch::deliver_pending_signal(sched::task* self,
                                                       int64_t result,
                                                       uint64_t syscall_num) {
    // A staged full-register return supersedes the frame, so it no longer
    // describes the resume context, delivery waits for a later boundary
    if (__atomic_load_n(&self->exec.iret_pending, __ATOMIC_ACQUIRE)) {
        return result;
    }

    x86::syscall_frame* ctx = x86::current_syscall_frame();

    // Delivery only when returning to user mode, an elevated task keeps
    // its signals pending until it lowers
    uint32_t sig = 0;
    signals::k_sigaction act{};
    signals::sig_set_t old_blocked = 0;
    bool delivered = !(self->exec.flags & sched::TASK_FLAG_ELEVATED) &&
        signals::take_deliverable(self, &sig, &act, &old_blocked);

    if (!delivered) {
        // Interrupted with nothing to run: restart transparently so a raced
        // or elevated boundary never surfaces a spurious EINTR
        if (result == syscall::ERESTARTSYS) {
            ctx->rip -= x86::SYSCALL_INSN_LEN;
            return static_cast<int64_t>(syscall_num);
        }
        return result;
    }

    // The handler returns through the restorer's rt_sigreturn,
    // an action installed without one is undeliverable.
    if (!(act.flags & signals::SA_RESTORER) || !act.restorer) {
        signals::die_from_signal(signals::SIGSEGV);
    }

    // SA_RESTART resumes at the SYSCALL insn with the number back in RAX,
    // re-executing the interrupted call once the handler returns
    int64_t saved_result = result;
    if (result == syscall::ERESTARTSYS) {
        if (act.flags & signals::SA_RESTART) {
            ctx->rip -= x86::SYSCALL_INSN_LEN;
            saved_result = static_cast<int64_t>(syscall_num);
        } else {
            saved_result = syscall::EINTR;
        }
    }

    if (x86::build_signal_frame(ctx, sig, &act, old_blocked,
                                saved_result) != 0) {
        signals::die_from_signal(signals::SIGSEGV);
    }
    return saved_result;
}

__PRIVILEGED_CODE int64_t arch::restore_signal_context() {
    return x86::restore_signal_frame(x86::current_syscall_frame());
}
