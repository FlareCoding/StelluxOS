#include "signal/delivery.h"
#include "arch/arch_signal.h"
#include "sched/fpu.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "signals/signal.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

namespace aarch64 {

// PSTATE condition flags (NZCV). Everything else is forced so a restored
// context returns to EL0t (mode bits 0) with interrupts unmasked (DAIF 0).
constexpr uint64_t SPSR_NZCV_MASK = 0xF0000000ULL;

static inline uint64_t align_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

static inline void copy_vregs(uint8_t dst[32][16], const uint8_t src[32][16]) {
    for (uint32_t i = 0; i < 32; i++) {
        for (uint32_t j = 0; j < 16; j++) {
            dst[i][j] = src[i][j];
        }
    }
}

__PRIVILEGED_CODE void pack_sigframe(rt_sigframe* frame, const trap_frame* tf,
                                     int64_t saved_result, uint32_t sig,
                                     signals::sig_set_t old_blocked,
                                     const sched::fpu_state* fp) {
    sigcontext& sc = frame->uc.uc_mcontext;
    for (uint32_t i = 0; i < 31; i++) {
        sc.regs[i] = tf->x[i];
    }

    sc.regs[0] = static_cast<uint64_t>(saved_result);
    sc.sp = tf->sp;
    sc.pc = tf->elr;
    sc.pstate = tf->spsr;
    sc.fault_address = tf->far;

    fpsimd_context* fc = reinterpret_cast<fpsimd_context*>(sc.__reserved);
    fc->magic = FPSIMD_MAGIC;
    fc->size = sizeof(fpsimd_context);
    fc->fpsr = fp->fpsr;
    fc->fpcr = fp->fpcr;
    copy_vregs(fc->vregs, fp->vregs);

    frame->uc.uc_sigmask = old_blocked;
    frame->info.si_signo = static_cast<int32_t>(sig);
    frame->info.si_code = SI_USER;
}

__PRIVILEGED_CODE bool unpack_sigframe(const rt_sigframe* frame, trap_frame* tf,
                                       sched::fpu_state* fp,
                                       signals::sig_set_t* mask) {
    const sigcontext& sc = frame->uc.uc_mcontext;
    const fpsimd_context* fc = reinterpret_cast<const fpsimd_context*>(sc.__reserved);
    if (fc->magic != FPSIMD_MAGIC) {
        return false;
    }

    for (uint32_t i = 0; i < 31; i++) {
        tf->x[i] = sc.regs[i];
    }

    tf->sp = sc.sp;
    tf->elr = sc.pc;
    tf->spsr = sc.pstate & SPSR_NZCV_MASK;

    fp->fpsr = fc->fpsr;
    fp->fpcr = fc->fpcr;
    copy_vregs(fp->vregs, fc->vregs);

    *mask = frame->uc.uc_sigmask;
    return true;
}

__PRIVILEGED_CODE int32_t build_signal_frame(trap_frame* tf, uint32_t sig,
                                             const signals::k_sigaction* act,
                                             signals::sig_set_t old_blocked,
                                             int64_t saved_result) {
    uint64_t frame_addr = align_down(tf->sp - sizeof(rt_sigframe), 16);

    rt_sigframe* frame = heap::kalloc_new<rt_sigframe>();
    if (!frame) {
        return -1;
    }
    sched::fpu_state fp;
    fpu::save(&fp);
    pack_sigframe(frame, tf, saved_result, sig, old_blocked, &fp);

    int32_t rc = mm::uaccess::copy_to_user(
        reinterpret_cast<void*>(frame_addr), frame, sizeof(*frame));
    heap::kfree_delete(frame);

    if (rc != mm::uaccess::OK) {
        return rc;
    }

    tf->sp = frame_addr;
    tf->elr = act->handler;
    tf->x[0] = sig;
    tf->x[1] = frame_addr + __builtin_offsetof(rt_sigframe, info);
    tf->x[2] = frame_addr + __builtin_offsetof(rt_sigframe, uc);
    tf->x[30] = act->restorer; // LR, the handler returns into the restorer
    return 0;
}

__PRIVILEGED_CODE int64_t restore_signal_frame(trap_frame* tf) {
    uint64_t frame_addr = tf->sp;

    rt_sigframe* frame = heap::kalloc_new<rt_sigframe>();
    if (!frame) {
        signals::die_from_signal(signals::SIGSEGV);
    }
    if (mm::uaccess::copy_from_user(
            frame, reinterpret_cast<void*>(frame_addr), sizeof(*frame)) != mm::uaccess::OK) {
        heap::kfree_delete(frame);
        signals::die_from_signal(signals::SIGSEGV);
    }

    sched::fpu_state fp;
    signals::sig_set_t mask = 0;
    if (!unpack_sigframe(frame, tf, &fp, &mask)) {
        heap::kfree_delete(frame);
        signals::die_from_signal(signals::SIGSEGV);
    }

    signals::set_blocked(sched::current(), signals::SIG_SETMASK, &mask, nullptr);
    fpu::restore(&fp);

    int64_t resume = static_cast<int64_t>(tf->x[0]);
    heap::kfree_delete(frame);

    return resume;
}

} // namespace aarch64

__PRIVILEGED_CODE int64_t arch::deliver_pending_signal(sched::task* self,
                                                       int64_t result) {
    uint32_t sig = 0;
    signals::k_sigaction act{};
    signals::sig_set_t old_blocked = 0;
    if (!signals::take_deliverable(self, &sig, &act, &old_blocked)) {
        return result;
    }

    // Same restorer contract as x86_64, the bundled musl always passes one
    if (!(act.flags & signals::SA_RESTORER) || !act.restorer) {
        signals::die_from_signal(signals::SIGSEGV);
    }

    if (aarch64::build_signal_frame(aarch64::current_trap_frame(), sig, &act,
                                    old_blocked, result) != 0) {
        signals::die_from_signal(signals::SIGSEGV);
    }

    // The dispatcher writes this into x0, matching the frame's sig argument
    return static_cast<int64_t>(sig);
}

__PRIVILEGED_CODE int64_t arch::restore_signal_context() {
    return aarch64::restore_signal_frame(aarch64::current_trap_frame());
}
